#include "storage/buffer/buffer_internal.h"

#include <string.h>

/*
 * Buffer frames.
 *
 * A frame is a page buffer and the four or five facts the pool has to remember about
 * the page in it. That is the whole abstraction, and keeping it this small is what
 * makes the two other modules readable: the clock chooses between frames, and it only
 * needs to know which are pinned and which have been used.
 *
 * The two state transitions, claim and release, live here rather than in the pool so
 * that there is exactly one place that assigns `page_id` and exactly one place that
 * decides what a free frame's buffer contains. Both are documented in
 * buffer_internal.h; the part worth repeating is the asymmetry between them:
 *
 *   claim    zeroes the buffer
 *   release  does not
 *
 * Zeroing on release would be a second page-sized memset on every eviction, which in a
 * pool of any size is a measurable cost paid on the one path that is already doing I/O.
 * Zeroing on claim costs at most one redundant memset on a page that is about to be
 * overwritten by a read anyway, and it means a caller that reads a freshly allocated
 * page sees zeros rather than the previous occupant's data.
 *
 * Ownership
 * ---------
 * The frame table owns the frame array and every page buffer in it, and owns them for
 * as long as the table exists. A caller that fetches a page receives a borrowed
 * `astra_page *` into a frame; it must not release it, and the only way the frame's
 * memory is reclaimed is astra_frame_table_destroy, which the pool calls exactly once
 * on its way out.
 */

/*
 * ---------------------------------------------------------------------------
 * Construction
 * ---------------------------------------------------------------------------
 */

/*
 * One page-sized allocation per frame, rather than one block of frames and one block
 * of buffers.
 *
 * A single block would be marginally faster to allocate and would put every frame on
 * the same cache lines' worth of alignment, but it makes a pool that cannot be fully
 * constructed impossible to release cleanly: a failure halfway down leaves a partial
 * array to unwind, and the caller has no idea which prefix is live. One allocation per
 * frame means the table is either wholly constructed or wholly absent, which is the
 * property astra_buffer_pool_create documents.
 */
astra_status astra_frame_table_init(astra_frame_table *table,
                                   uint32 count,
                                   uint32 page_size)
{
    uint32 index;
    astra_status status;

    if (table == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    table->frames = NULL;
    table->count = 0;
    table->fresh_next = 0;
    table->used = 0;

    /*
     * The rule for every count-taking entry point in AstraDB: a null pointer is a bad
     * argument, and a value that is the right type but outside the range this build
     * honours is unsupported. Zero frames is the latter - it is a well-formed number, and
     * a pool of zero frames is not a pool, so there is nothing to create.
     */
    if (count == 0u) {
        return ASTRA_ERR_UNSUPPORTED;
    }
    if (!astra_page_size_is_valid(page_size)) {
        return ASTRA_ERR_UNSUPPORTED;
    }

    table->frames = astra_alloc_zeroed(count, sizeof *table->frames);
    if (table->frames == NULL) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    /*
     * Allocate every page buffer before handing the table back. A half-built table
     * whose frame 12 has no buffer would be found by the first eviction that reached
     * it, which is a failure report about a buffer pool caused by a constructor that
     * reported success. Unwinding here is the only place that has to know how to.
     */
    for (index = 0u; index < count; ++index) {
        status = astra_page_init(&table->frames[index].page, page_size);
        if (status != ASTRA_OK) {
            while (index > 0u) {
                --index;
                astra_page_release(&table->frames[index].page);
            }
            astra_dealloc(table->frames);
            table->frames = NULL;
            return status;
        }

        /*
         * astra_page_init leaves page_id at ASTRA_PAGE_ID_INVALID and both flags
         * false, so only the pin count and the chain link need setting. Written out
         * rather than assumed, because the invariant that a free frame is not in the
         * page table is load-bearing and should not rest on a struct initialiser in
         * another translation unit.
         */
        table->frames[index].page_id = ASTRA_PAGE_ID_INVALID;
        table->frames[index].pin_count = 0u;
        table->frames[index].is_dirty = false;
        table->frames[index].is_referenced = false;
        table->frames[index].is_loading = false;
        table->frames[index].write_latched = false;
        table->frames[index].chain_next = ASTRA_FRAME_NONE;
        table->frames[index].in_table = false;
    }

    table->count = count;
    return ASTRA_OK;
}

void astra_frame_table_destroy(astra_frame_table *table)
{
    uint32 index;

    if (table == NULL || table->frames == NULL) {
        return;
    }

    for (index = 0u; index < table->count; ++index) {
        astra_page_release(&table->frames[index].page);
    }

    astra_dealloc(table->frames);
    table->frames = NULL;
    table->count = 0u;
    table->fresh_next = 0u;
    table->used = 0u;
}

/*
 * ---------------------------------------------------------------------------
 * The two transitions
 * ---------------------------------------------------------------------------
 */

void astra_frame_claim(astra_buffer_frame *frame, page_id_t page_id)
{
    /*
     * The buffer is cleared rather than left as the previous page left it. The Disk
     * Manager zero-fills a newly allocated page, so a page that is allocated and never
     * written reads back as zeros; a frame that was recycled from a page with real
     * contents would otherwise make the same page look non-empty, and the difference
     * is exactly the kind that only shows up as wrong data much later.
     *
     * astra_page_clear cannot fail here: the buffer was allocated by this module with
     * exactly this page size, and the frame is one this module handed out. The status
     * is discarded with (void) rather than propagated because a claim that returned a
     * status would have to be checked at four call sites for a condition this module
     * guarantees cannot occur.
     */
    (void)astra_page_clear(&frame->page, frame->page.data_size);

    /*
     * Both identifiers, together, in the one function that is allowed to touch them.
     * `frame->page_id` is the key the page table searches on and `frame->page.page_id`
     * is what the Disk Manager checks a write against; setting them in one place is
     * what makes "a frame's page identifier is the page it holds" true rather than
     * usually true.
     */
    frame->page_id = page_id;
    frame->page.page_id = page_id;

    frame->page.is_dirty = false;
    frame->pin_count = 1u;
    frame->is_dirty = false;
    frame->is_loading = false;

    /*
     * The reference bit is cleared rather than left as it was found.
     *
     * An earlier version left it alone, on the reasoning that the clock clears the bit
     * before handing a frame over, so clearing it again could only be redundant. That is
     * true of the eviction path and only of the eviction path: astra_frame_claim is also
     * how a frame is bound to a freshly allocated page, and how a frame is re-bound after
     * a new page's disk allocation, and in neither of those cases has anything set the
     * bit. A stale bit would then be carried into the new page's life, marking a page as
     * used that has never been read.
     *
     * Clearing it here makes the function self-contained: afterwards, the bit means "the
     * page in this frame has been used", and the only thing that can have set it is a
     * call to astra_page_record_access. Callers that have just loaded or created a page
     * record the access themselves, immediately afterwards.
     */
    frame->is_referenced = false;
}

void astra_frame_release(astra_buffer_frame *frame)
{
    /*
     * page_id is the discriminator for "this frame holds nothing", so it is the one
     * field that must be set. Everything else is reset because a free frame that kept
     * a stale pin count would be skipped by the clock forever and the pool would report
     * itself full while holding a frame nobody could evict.
     */
    frame->page_id = ASTRA_PAGE_ID_INVALID;
    frame->page.page_id = ASTRA_PAGE_ID_INVALID;
    frame->page.is_dirty = false;
    frame->pin_count = 0u;
    frame->is_dirty = false;
    frame->is_loading = false;
    frame->write_latched = false;
    /*
     * The reference bit too, and this one is a bug rather than a nicety. It is the flag
     * the clock reads to decide whether a frame is owed a second chance, so a free frame
     * that kept a stale set bit would be examined, found "recently used", and cleared -
     * costing a sweep step on a frame that holds nothing. Clearing it here is what makes
     * "a free frame is a blank slate" true of every field rather than all but one.
     */
    frame->is_referenced = false;
    frame->chain_next = ASTRA_FRAME_NONE;
}

bool astra_frame_is_resident(const astra_buffer_frame *frame)
{
    if (frame == NULL) {
        return false;
    }
    return frame->page_id != ASTRA_PAGE_ID_INVALID;
}

/*
 * ---------------------------------------------------------------------------
 * The page table
 * ---------------------------------------------------------------------------
 *
 * Documented in full in buffer_internal.h. The part that is easy to get wrong and is
 * therefore spelled out here: the links live in the frames, so every operation has to
 * be given both the table and the frame array, and a removal has to fix up the *link
 * in the frame before the removed one*. Forgetting that is not a crash; it is a
 * bucket that still points at a frame now holding a different page, and the symptom is
 * a fetch that returns the wrong page's bytes.
 */

uint64 astra_page_hash(page_id_t page_id)
{
    /*
     * The splitmix64 finaliser. Chosen for three reasons: it is a bijection, so two
     * identifiers can never collide *by construction*; it needs no table, so there is
     * no initialisation order and no state to guard; and it mixes the high bits down
     * into the low ones, which matters because page identifiers are dense and a hash
     * that only touched the low bits would put every page in one bucket.
     *
     * The constants are the ones published with the algorithm; there is nothing to
     * derive and nothing to tune.
     */
    uint64 value = page_id + UINT64_C(0x9e3779b97f4a7c15);

    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

/*
 * Rounds `value` up to the next power of two, or reports that it cannot.
 *
 * A power-of-two bucket count turns the bucket index into a bitwise and, which is the
 * difference between a fetch that costs a multiply and a fetch that costs a shift.
 * Small values pass through unchanged; anything above 2^31 is refused, because the
 * bucket array is indexed by a uint32 and a count that does not fit in one could not
 * be allocated anyway.
 */
static bool round_up_pow2(uint64 value, uint32 *out)
{
    uint32 result = 1u;

    if (value == 0u) {
        *out = 1u;
        return true;
    }

    while ((uint64)result < value) {
        if (result > (UINT32_MAX >> 1)) {
            return false;
        }
        result <<= 1;
    }

    *out = result;
    return true;
}

astra_status astra_page_table_init(astra_page_table *table, uint32 frame_count)
{
    uint64 wanted;
    uint32 buckets;
    uint32 index;

    if (table == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (frame_count == 0u) {
        return ASTRA_ERR_UNSUPPORTED;
    }

    table->buckets = NULL;
    table->bucket_count = 0u;
    table->frame_limit = 0u;
    table->entries = 0u;

    /*
     * Two buckets per frame, so a pool that is completely full averages one and a half
     * entries per bucket. The alternative - one bucket per frame - has the same
     * asymptotic cost and half the memory, and the reason for the extra factor is that
     * the table is allocated once and must never be reallocated: at one bucket per
     * frame, a pool filling up approaches a chain length of one, and at a pool of four
     * thousand pages that is a four kilobyte array saved for a hash table that is
     * read on every single fetch. The cost is a few kilobytes; the benefit is that the
     * memory is never touched again.
     */
    wanted = (uint64)frame_count * 2u;
    if (wanted < 8u) {
        wanted = 8u;
    }

    if (!round_up_pow2(wanted, &buckets)) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    table->buckets = astra_alloc_zeroed(buckets, sizeof *table->buckets);
    if (table->buckets == NULL) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    /*
     * An empty bucket is ASTRA_FRAME_NONE, which is deliberately not zero, so the
     * zeroed allocation has to be overwritten. This is the one place the table pays for
     * that choice: one pass over the buckets at construction, in exchange for every
     * lookup afterwards being able to trust the value it reads.
     */
    for (index = 0u; index < buckets; ++index) {
        table->buckets[index] = ASTRA_FRAME_NONE;
    }

    table->bucket_count = buckets;
    table->frame_limit = frame_count;
    return ASTRA_OK;
}

void astra_page_table_destroy(astra_page_table *table)
{
    if (table == NULL) {
        return;
    }

    astra_dealloc(table->buckets);
    table->buckets = NULL;
    table->bucket_count = 0u;
    table->frame_limit = 0u;
    table->entries = 0u;
}

uint32 astra_page_table_find(const astra_page_table *table,
                             const astra_buffer_frame *frames,
                             page_id_t page_id)
{
    uint32 index;
    uint32 guard;

    if (table == NULL || frames == NULL || table->buckets == NULL) {
        return ASTRA_FRAME_NONE;
    }
    if (!astra_page_id_is_valid(page_id)) {
        return ASTRA_FRAME_NONE;
    }

    index = table->buckets[(uint32)(astra_page_hash(page_id)
                                    & (uint64)(table->bucket_count - 1u))];

    /*
     * The chain cannot be longer than the frame count, so `guard` bounds a walk that
     * has been broken by a bug rather than by anything a caller can do. Returning
     * "not found" from a corrupted table turns an infinite loop into a failed fetch,
     * which is a strictly better failure mode, and costs one comparison per step.
     */
    for (guard = 0u; guard < table->bucket_count; ++guard) {
        if (index == ASTRA_FRAME_NONE) {
            return ASTRA_FRAME_NONE;
        }
        if (frames[index].page_id == page_id) {
            return index;
        }
        index = frames[index].chain_next;
    }

    return ASTRA_FRAME_NONE;
}

bool astra_page_table_insert(astra_page_table *table,
                             astra_buffer_frame *frames,
                             page_id_t page_id,
                             uint32 frame_index)
{
    uint32 bucket;

    if (table == NULL || frames == NULL || table->buckets == NULL) {
        return false;
    }
    if (!astra_page_id_is_valid(page_id)) {
        return false;
    }

    /*
     * Two checks, both of which are caller bugs rather than conditions to handle, and
     * both of which happen before anything is modified so that a false return always
     * leaves the table exactly as it was.
     */
    if (frame_index >= table->frame_limit) {
        return false;
    }
    if (frames[frame_index].page_id != page_id) {
        return false;
    }
    if (astra_page_table_find(table, frames, page_id) != ASTRA_FRAME_NONE) {
        return false;
    }

    /*
     * Push at the head of the chain. Walking the chain to find its tail would touch
     * every colliding frame on every insert, and the head is as good a place as any:
     * the most recently cached page in a bucket is also the most likely to be the next
     * one fetched from it.
     *
     * The order of the two assignments matters. The frame's link is set before the
     * bucket points at it, so at no instant does the bucket head name a frame whose
     * chain_next is still the link it had before. The pool latch is held throughout, so
     * no other thread is walking the chain at all - but the invariant is worth keeping
     * true by construction rather than by argument.
     */
    bucket = (uint32)(astra_page_hash(page_id) & (uint64)(table->bucket_count - 1u));

    frames[frame_index].chain_next = table->buckets[bucket];
    table->buckets[bucket] = frame_index;
    ++table->entries;
    frames[frame_index].in_table = true;

    return true;
}

bool astra_page_table_remove(astra_page_table *table,
                             astra_buffer_frame *frames,
                             page_id_t page_id)
{
    uint32 bucket;
    uint32 current;
    uint32 previous;
    uint32 guard;

    if (table == NULL || frames == NULL || table->buckets == NULL) {
        return false;
    }

    bucket = (uint32)(astra_page_hash(page_id) & (uint64)(table->bucket_count - 1u));

    if (table->buckets[bucket] == ASTRA_FRAME_NONE) {
        return false;
    }

    /* Case one: the frame is at the head of the chain. */
    current = table->buckets[bucket];
    if (frames[current].page_id == page_id) {
        table->buckets[bucket] = frames[current].chain_next;
        frames[current].chain_next = ASTRA_FRAME_NONE;
        frames[current].in_table = false;
        --table->entries;
        return true;
    }

    /* Case two: somewhere further down. */
    previous = current;
    current = frames[current].chain_next;
    for (guard = 1u; guard < table->bucket_count; ++guard) {
        if (current == ASTRA_FRAME_NONE) {
            return false;
        }
        if (frames[current].page_id == page_id) {
            frames[previous].chain_next = frames[current].chain_next;
            frames[current].chain_next = ASTRA_FRAME_NONE;
            frames[current].in_table = false;
            --table->entries;
            return true;
        }
        previous = current;
        current = frames[current].chain_next;
    }

    return false;
}

uint64 astra_page_table_count(const astra_page_table *table)
{
    if (table == NULL) {
        return 0u;
    }
    return table->entries;
}

/*
 * ---------------------------------------------------------------------------
 * The retired identifier set
 * ---------------------------------------------------------------------------
 */

void astra_retired_set_init(astra_retired_set *set)
{
    if (set == NULL) {
        return;
    }
    set->ids = NULL;
    set->count = 0u;
    set->capacity = 0u;
}

void astra_retired_set_destroy(astra_retired_set *set)
{
    if (set == NULL) {
        return;
    }
    astra_dealloc(set->ids);
    set->ids = NULL;
    set->count = 0u;
    set->capacity = 0u;
}

/*
 * Returns the position of `page_id` in the sorted array, and whether it is there.
 *
 * Written as a function returning a pair through pointers rather than as a lookup that
 * returns the index and a separate contains, because every caller needs both: insert
 * needs the position to memmove into, and contains only needs the flag.
 */
static bool retired_search(const astra_retired_set *set,
                           page_id_t page_id,
                           uint64 *out_position)
{
    uint64 low = 0u;
    uint64 high = set->count;

    while (low < high) {
        uint64 middle = low + (high - low) / 2u;

        if (set->ids[middle] == page_id) {
            *out_position = middle;
            return true;
        }
        if (set->ids[middle] < page_id) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }

    *out_position = low;
    return false;
}

bool astra_retired_set_contains(const astra_retired_set *set, page_id_t page_id)
{
    uint64 position;

    if (set == NULL || set->count == 0u) {
        return false;
    }
    return retired_search(set, page_id, &position);
}

astra_status astra_retired_set_add(astra_retired_set *set, page_id_t page_id)
{
    uint64 position;
    page_id_t *grown;
    uint64 capacity;

    if (set == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (!astra_page_id_is_valid(page_id)) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    if (retired_search(set, page_id, &position)) {
        /* Already retired. Not an error: the public delete accepts a page the pool
         * does not hold, so deleting one twice is a legal way to end up here. */
        return ASTRA_OK;
    }

    if (set->count == set->capacity) {
        /* Start at eight so a database that deletes a handful of pages does not
         * reallocate on every one of them. */
        if (set->capacity == 0u) {
            capacity = 8u;
        } else {
            if (set->capacity > (uint64)(SIZE_MAX / 2u / sizeof *set->ids)) {
                /* Doubling would overflow either the capacity counter or the byte
                 * count. Both are checked with one comparison because the alternative
                 * is a wrapped capacity producing a heap overflow on the next insert. */
                return ASTRA_ERR_OUT_OF_MEMORY;
            }
            capacity = set->capacity * 2u;
        }

        if (capacity > (uint64)(SIZE_MAX / sizeof *set->ids)) {
            return ASTRA_ERR_OUT_OF_MEMORY;
        }

        grown = astra_realloc(set->ids, (size_t)capacity * sizeof *set->ids);
        if (grown == NULL) {
            /* astra_realloc leaves the old block owned by this set and still valid, so
             * a failed add is a no-op and the caller can report it and carry on with a
             * pool whose retired set is unchanged. */
            return ASTRA_ERR_OUT_OF_MEMORY;
        }
        set->ids = grown;
        set->capacity = capacity;
    }

    if (position < set->count) {
        memmove(&set->ids[position + 1u],
                &set->ids[position],
                (size_t)(set->count - position) * sizeof *set->ids);
    }
    set->ids[position] = page_id;
    ++set->count;

    return ASTRA_OK;
}
