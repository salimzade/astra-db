/*
 * AstraDB storage: the Buffer Pool's internals.
 *
 * Private. Not installed, not part of the public API. The public Buffer Pool
 * contract is include/astra/storage/buffer_pool.h; this header is the shape of the
 * thing that contract describes, shared by the three modules that make it up.
 *
 * ---------------------------------------------------------------------------
 * Why three modules
 * ---------------------------------------------------------------------------
 *   buffer_frame.c  one frame and the table of them. The unit that owns a page
 *                   buffer and knows what a pin count and a dirty bit mean.
 *   replacer.c      the clock. Given the frames, choose one to give up. Knows
 *                   nothing about pages, pages' identifiers or the disk.
 *   buffer_pool.c   the public entry points, the page table, and the latch.
 *
 * The split is by what each one has to know, not by file size. The clock is the part
 * that would be replaced by a different policy, and it is worth being able to read it
 * without reading the page table.
 *
 * ---------------------------------------------------------------------------
 * Locking discipline
 * ---------------------------------------------------------------------------
 * There is exactly one latch, `astra_buffer_pool::latch`, and it guards every field
 * of the pool and of every frame. Two rules make that correct, and they are the only
 * two:
 *
 *   1. The latch is never held across a call into the Disk Manager. A read or a write
 *      of a page is milliseconds of system time; holding a latch across one would
 *      make every unrelated operation in the process wait for the slowest disk on
 *      the machine.
 *
 *   2. Anything the latch protects and that a thread intends to use outside the latch
 *      is *pinned* first, and released last. A pinned frame cannot be evicted, so it
 *      cannot be recycled underneath a thread that is reading from disk into it or
 *      writing its bytes out.
 *
 * Rule 2 is what makes rule 1 safe, and it is why the loader pins the frame it is
 * loading *before* it drops the latch, rather than after the read returns.
 *
 * The one place a second latch appears is `astra_buffer_frame::write_latched`, which
 * serialises two flushes of the same page against each other. It is a boolean rather
 * than an `astra_mutex` because the only thing two flushes of one page must not do is
 * run at the same time; the pool's own latch cannot be held to guarantee that
 * without serialising every write in the pool behind the slowest one.
 *
 * Lock order: the pool latch, and nothing else. No code path acquires two latches,
 * so there is no order to get wrong and no cycle to detect. When transactions arrive
 * and a page latch is added above this one, the order is: pool latch, then page
 * latch, and never the reverse.
 *
 * ---------------------------------------------------------------------------
 * Frame lifecycle, in one place
 * ---------------------------------------------------------------------------
 * A frame is in exactly one of two states, and `page_id` is the discriminator:
 *
 *   page_id == ASTRA_PAGE_ID_INVALID   the frame is free; its buffer's contents are
 *                                     whatever the previous occupant left there.
 *   page_id != ASTRA_PAGE_ID_INVALID   the frame holds that page, and it is in the
 *                                     page table unless it is being evicted.
 *
 * `astra_frame_claim` and `astra_frame_release` are the only transitions, and they
 * are the only code that assigns `page_id`. A frame that is free is not in the page
 * table; a frame that holds a page always is, except for the window an eviction opens
 * on purpose. That window is why `in_table` exists as a separate flag: "holds a page"
 * and "can be found by name" are different questions during it, and the flush sweeps,
 * which walk the frame array by index, have to be able to tell them apart.
 */
#ifndef ASTRA_STORAGE_BUFFER_BUFFER_INTERNAL_H
#define ASTRA_STORAGE_BUFFER_BUFFER_INTERNAL_H

#include "astra/core/allocator.h"
#include "astra/core/config.h"
#include "astra/core/error.h"
#include "astra/core/log.h"
#include "astra/core/types.h"
#include "astra/storage/buffer_pool.h"
#include "astra/storage/disk_manager.h"
#include "astra/storage/page.h"
#include "core/sync.h"

#include <stddef.h>

/*
 * ---------------------------------------------------------------------------
 * The sentinel used for "this frame holds no page"
 * ---------------------------------------------------------------------------
 *
 * A frame is addressed by its index, and index 0 is a real frame - the first frame a pool
 * ever hands out. So "no frame" cannot be 0, and this sentinel is the largest uint32
 * instead.
 *
 * An earlier version of this file used the opposite convention: an "index plus one"
 * encoding, so that a zero-initialised bucket array read as a chain of "none" and every
 * link was stored one higher than the index it named. It looked like it removed a class
 * of bug, and it did - but it introduced a worse one, because `find` then had no way to
 * say "the page is in frame 0" differently from "the page is not in the table at all". The
 * fix is not a cleverer encoding but a sentinel that no index can collide with, and a
 * table that is initialised to it rather than relying on the allocator.
 */
#define ASTRA_FRAME_NONE ((uint32)0xFFFFFFFFu)

/** Largest frame count a pool may have; one past the largest legal index. */
#define ASTRA_FRAMES_LIMIT ((uint64)ASTRA_BUFFER_POOL_PAGES_MAX + 1u)

/** Largest legal frame index, as a uint32. */
#define ASTRA_FRAME_INDEX_MAX ((uint32)(ASTRA_BUFFER_POOL_PAGES_MAX - 1u))

/*
 * The clock's sweep bound is computed as `frame_count * 2` in a uint32. This asserts
 * that it cannot wrap, so the bound stays correct if ASTRA_BUFFER_POOL_PAGES_MAX is
 * ever raised rather than quietly becoming a much shorter sweep.
 */
ASTRA_STATIC_ASSERT(((uint64)ASTRA_BUFFER_POOL_PAGES_MAX * 2u) <= (uint64)UINT32_MAX,
                    "the clock's sweep bound must not overflow a uint32");

/*
 * ---------------------------------------------------------------------------
 * Frames
 * ---------------------------------------------------------------------------
 */

/**
 * One slot in the pool: a page buffer plus everything the pool has to remember
 * about the page currently in it.
 *
 * The struct is private to the library, and every field is guarded by the owning
 * pool's latch. `buffer_pool_fetch_page` hands the caller a pointer to `page` and to
 * nothing else in here, which is what lets the pool change any other field at any
 * time without the caller being able to observe it.
 */
typedef struct astra_buffer_frame {
    /** The page in this frame, or ASTRA_PAGE_ID_INVALID when the frame is free. */
    page_id_t page_id;

    /**
     * The page buffer. Owned by the frame for the life of the frame.
     *
     * `page.page_id` is always equal to the frame's own `page_id`. They are two fields
     * because one of them belongs to the public `astra_page` value and the other
     * belongs to the frame, and the Disk Manager will not write a page whose own
     * identifier disagrees with where it is going. astra_frame_claim and
     * astra_frame_release are the only functions that assign either, so the two can
     * never drift.
     */
    astra_page page;

    /** How many callers hold this frame. Zero means it may be evicted. */
    uint32 pin_count;

    /** True when the buffer differs from what is on disk. */
    bool is_dirty;

    /** The clock's reference bit: true when the page was used since it was swept. */
    bool is_referenced;

    /**
     * True while a thread is reading this page from disk into the frame.
     *
     * A second fetch of the same page waits rather than starting a second load into
     * the same buffer, which would leave the page holding neither version.
     */
    bool is_loading;

    /**
     * True while a thread holds this frame's write latch.
     *
     * A flusher sets it before dropping the pool latch and clears it after the write
     * returns, so a second flusher of the same page waits instead of writing the same
     * bytes at the same time. It is a flag rather than a mutex because the pool latch
     * is what guards it: a waiter blocks in astra_latch_wait, which re-acquires the
     * latch in a loop and rechecks this flag, and the flusher broadcasts before it
     * releases the latch.
     *
     * It also keeps a frame out of circulation while its bytes are in flight. A fetch
     * that finds a resident page whose frame is write-latched waits rather than handing
     * out a pointer to a buffer that is being overwritten, and an eviction that found
     * the frame would likewise be flushing a page it is in the middle of replacing.
     */
    bool write_latched;

    /**
     * Next frame in this page table bucket chain; ASTRA_FRAME_NONE ends it.
     *
     * A raw index, not an encoded one. The chain lives in the frames rather than in the
     * table's buckets so that removing a page is a pointer move instead of a tombstone.
     * A table that accumulates tombstones either needs a rebuild, and therefore an
     * allocation on the eviction path, or its probe chains grow without bound. See
     * astra_page_table for the reasoning.
     */
    uint32 chain_next;

    /**
     * True exactly when this frame is linked into the page table, and therefore exactly
     * when some lookup can name it.
     *
     * This is not the same question as "does this frame hold a page", and conflating the
     * two is a bug that only shows up under contention. `page_id` stays set across the
     * whole of an eviction, because pool_write_frame needs the identity it is writing
     * *from*; between the unlink and the claim, therefore, the frame holds a page that
     * nothing can reach. Any code that wants "can this be found by name" - the flush
     * sweeps above all, which walk the frame array by index rather than by lookup - has to
     * ask this flag, not `page_id`.
     *
     * Maintained in astra_page_table_insert and astra_page_table_remove and nowhere else,
     * which is what keeps it true by construction rather than by argument: those two
     * functions are the only code that changes the table, so the flag cannot drift from it.
     */
    bool in_table;
} astra_buffer_frame;

/**
 * A pool's frames, plus the two cursors that hand them out.
 *
 * Allocated once, at construction, and never resized. `fresh_next` is a cursor, not a
 * free list: because a frame is only ever reused after being evicted, and eviction
 * only happens once every frame is in use, `fresh_next` walks 0, 1, 2, ... and stops.
 * That is the whole allocation policy for cold frames.
 */
typedef struct astra_frame_table {
    /** `count` frames, contiguous. Owned. Never NULL while the pool exists. */
    astra_buffer_frame *frames;

    /** Number of frames; the pool's capacity. */
    uint32 count;

    /** Next frame index that has never held a page. Equals `count` when exhausted. */
    uint32 fresh_next;

    /** How many frames currently hold a page. Never exceeds `count`. */
    uint64 used;
} astra_frame_table;

/*
 * ---------------------------------------------------------------------------
 * The page table
 * ---------------------------------------------------------------------------
 *
 * A page identifier is dense and unbounded - page 10,000,000 is a perfectly ordinary
 * identifier - while the number of pages the pool can hold is fixed at construction.
 * So the map from identifier to frame cannot be an array indexed by identifier, and it
 * cannot be a linear scan either, because the scan would run over every cached page on
 * every fetch. It is a hash table.
 *
 * Separate chaining, with the links stored in the frames themselves, which is the one
 * decision that shapes the rest:
 *
 *   - Removal is a pointer move. A frame that is being evicted is unlinked from its
 *     chain by relinking the frame before it. There is no tombstone to leave behind,
 *     so a table that has seen a million evictions looks exactly like a new one.
 *   - Nothing on the fetch or the eviction path allocates. A tombstone-based table
 *     either grows its chains forever or needs a rehash, and a rehash can fail for
 *     want of memory on exactly the path where a caller cannot do anything about it.
 *   - There is no key stored in the table. The key is `frame->page_id`, which is
 *     already there and cannot disagree with the bucket the frame is linked into.
 *
 * The cost is a pointer chase per colliding key. With a table sized to at least two
 * frames and a decent hash, that is a cache miss on a fraction of lookups rather than
 * a scan, and it is bounded by the number of resident pages, never by the number of
 * pages in the file.
 */
typedef struct astra_page_table {
    /**
     * `bucket_count` bucket heads, each a raw frame index.
     *
     * ASTRA_FRAME_NONE marks an empty bucket. Owned. Never NULL while the pool exists, and
     * every entry is initialised to ASTRA_FRAME_NONE rather than left at whatever the
     * allocator returned - the whole point of the sentinel is that it is not zero.
     */
    uint32 *buckets;

    /** Number of buckets; always a power of two, so the mask is a bit and. */
    uint32 bucket_count;

    /** Number of frames the table was built for. Bounds every frame index it is given. */
    uint32 frame_limit;

    /** Number of frames linked into the table. Never exceeds `frame_limit`. */
    uint64 entries;
} astra_page_table;

/*
 * ---------------------------------------------------------------------------
 * The retired identifier set
 * ---------------------------------------------------------------------------
 *
 * `astra_buffer_pool_delete_page` has to remember that an identifier is dead for the
 * rest of the pool's life, so that a stale reference cannot resurrect a page whose
 * bytes are still in the file. The set is a sorted array rather than a hash table
 * because it is the only structure in the pool that grows, and because it is written
 * on a path that must not fail for want of memory: a delete that cannot be recorded is
 * a delete that has not happened, and the alternative to reporting the failure is to
 * serve a page the caller just deleted.
 *
 * Deletions are rare and bounded by how many pages a workload retires, so a binary
 * search per fetch and a memmove per delete is the right trade. A database that
 * deletes millions of pages pays for that on every fetch, and the fix at that point is
 * a free space map inside the file rather than a better in-memory structure: the set
 * is a property of the file, not of the pool, and the pool only holds it because there
 * is nowhere else for it to live yet.
 */
typedef struct astra_retired_set {
    /** `count` retired identifiers, ascending. Owned. NULL when empty. */
    page_id_t *ids;

    /** Number of retired identifiers. */
    uint64 count;

    /** Allocated capacity in identifiers. */
    uint64 capacity;
} astra_retired_set;

/*
 * ---------------------------------------------------------------------------
 * replacer.c - the clock
 * ---------------------------------------------------------------------------
 */

/** The clock's state. Owned by the pool; separate so the policy has no pool in it. */
typedef struct astra_replacer {
    /**
     * The frame the clock examines next.
     *
     * An index into the pool's frame array rather than a pointer, so the replacer holds
     * no memory of its own and cannot outlive the frames it is choosing between.
     */
    uint32 hand;
} astra_replacer;

/*
 * ---------------------------------------------------------------------------
 * The pool
 * ---------------------------------------------------------------------------
 */

/**
 * A pool of frames over one database.
 *
 * Every field below the latch is guarded by `latch`; nothing in this struct is
 * touched without it, with one exception: `replacement_count` is only ever
 * incremented, and only while the latch is held, so it needs no separate care.
 *
 * The struct is defined here rather than in the public header so that adding a
 * statistic, a counter or a field is not an ABI change and not a documented promise.
 */
struct astra_buffer_pool {
    /** Guards everything below. Never held across a Disk Manager call. */
    astra_latch latch;

    /** The frames. Owned. */
    astra_frame_table frames;

    /** Identifier to frame. Owned. */
    astra_page_table table;

    /** Identifiers that must never be served again. Owned. */
    astra_retired_set retired;

    /** The clock. Owned. Its only state is where the hand is. */
    astra_replacer replacer;

    /** The database. Borrowed; must outlive the pool. */
    astra_disk_manager *disk;

    /** Page size, cached from `disk` at construction. */
    uint32 page_size;

    /** Total clock sweeps, for diagnostics and for the replacement tests. */
    uint64 replacement_count;

    /**
     * Threads currently blocked in astra_latch_wait.
     *
     * Not needed for correctness - a waiter is woken by a broadcast and re-checks its
     * predicate - but it lets the tests assert that the pool really does block rather
     * than spin, and it is the difference between "no thread can proceed" and "no
     * thread is trying" being distinguishable from the outside.
     */
    uint32 waiters;
};

/*
 * ---------------------------------------------------------------------------
 * buffer_frame.c
 * ---------------------------------------------------------------------------
 */

/**
 * Allocates `count` frames, each with a zeroed page buffer of `page_size` bytes.
 *
 * On success `*out_table` owns the frames. On failure nothing is allocated: a pool
 * with thirty nine of the forty frames it wanted is not a smaller pool, it is a bug
 * that shows up much later as a page that cannot be cached.
 */
astra_status astra_frame_table_init(astra_frame_table *table,
                                   uint32 count,
                                   uint32 page_size);

/**
 * Releases every frame's buffer and the frame array itself.
 *
 * `table` must have come from astra_frame_table_init. NULL is allowed. Does not care
 * which frames hold pages: a pool destroys the whole table at once, after flushing, so
 * there is no per-frame teardown to forget.
 */
void astra_frame_table_destroy(astra_frame_table *table);

/**
 * Moves a frame from "free" to "holding page_id, pinned once, clean".
 *
 * Assigns both `frame->page_id` and `frame->page.page_id`, and clears the buffer,
 * because a recycled frame still holds the previous page's bytes and a caller that
 * reads a page before writing it must not see them. The buffer is not zeroed for a
 * *fetch* - the read that follows overwrites all of it - but the two callers do not
 * have to know that, and one memset that is sometimes redundant is cheaper than a flag
 * saying when it is not.
 *
 * The frame is not yet in the page table. The caller links it with
 * astra_page_table_insert while holding the same latch, so no other thread can observe
 * a bound-but-unlinked frame. `is_dirty` is set by the caller afterwards, for the
 * new-page path only.
 *
 * The reference bit is cleared, so that afterwards it means exactly "the page in this
 * frame has been used" and the only way to have set it is astra_page_record_access. The
 * eviction path clears the bit anyway when it chooses a victim, but astra_frame_claim is
 * also how a frame is bound to a freshly allocated page, where nothing has cleared
 * anything and a stale bit would mark a page as read before it was ever loaded. Callers
 * that have just loaded or created the page record the access themselves.
 */
void astra_frame_claim(astra_buffer_frame *frame, page_id_t page_id);


/**
 * Moves a frame from "holding a page" back to "free".
 *
 * The buffer is *not* cleared. A free frame's contents are undefined by contract and
 * the next claim clears it; clearing here would mean a second page-sized memset on
 * every eviction, which is the one cost a buffer pool must not add.
 */
void astra_frame_release(astra_buffer_frame *frame);

/** Returns true when `frame` holds a page, that is when `page_id` is meaningful. */
bool astra_frame_is_resident(const astra_buffer_frame *frame);

/*
 * ---------------------------------------------------------------------------
 * replacer.c - the clock
 * ---------------------------------------------------------------------------
 */

/**
 * Notes that `frame` was used, so the clock gives it a second chance when it reaches
 * it. Called on a fetch hit and on a load, which are the only two events that mean
 * "this page was wanted".
 */
void astra_replacer_record_access(astra_buffer_frame *frame);

/**
 * Chooses a frame to evict, or reports that there is none.
 *
 * The rule is the clock's: sweep from the hand, and for each frame take it if it is
 * unpinned and unreferenced, clear its reference bit and move on if it is unpinned and
 * referenced, and skip it if it is pinned. A pinned frame's reference bit is left
 * alone, so a page that was pinned across a long scan is still on its second chance
 * afterwards rather than being pushed to the back of the queue by being busy.
 *
 * At most two full sweeps, which is what makes a false return trustworthy. The first
 * sweep clears the reference bit of every unpinned frame it passes, so the second sweep
 * is guaranteed to find one if any unpinned frame exists at all. With only one sweep a
 * pool whose frames were all referenced since the last eviction would report "no
 * victim" while holding nothing it was allowed to throw away, and the caller's fetch
 * would fail with an error about pinned frames when none is pinned.
 *
 * The hand advances past the victim and is left where a fruitless sweep ended, which
 * is where it started, so a briefly full pool does not lose its place in the rotation.
 *
 * Parameters:
 *   replacer     - the clock. Not const: the hand is the clock's own state, and hiding
 *                  a mutation behind a const pointer would be a lie about what this
 *                  function does.
 *   frames       - the frames to choose among. Not const, for the same reason: clearing
 *                  a reference bit is the policy's work, not the caller's.
 *   frame_count  - number of frames. Must be at least one.
 *   out_examined - destination for the number of frames looked at, or NULL. Used for
 *                  the pool's sweep counter; not part of the decision.
 *   out_frame_index - destination for the victim's index. Must not be NULL. Written
 *                  only on success.
 *
 * Returns:
 *   true when a victim was found. It is unpinned and its reference bit is already clear,
 *   so the caller can load into it without setting anything.
 *   false when every frame is pinned, which after two complete sweeps is the only
 *   remaining explanation. `*out_frame_index` is then untouched.
 *
 * The caller must hold the pool latch. The replacer calls nothing, so it cannot block
 * and cannot be interrupted between choosing a frame and being told about it.
 */
bool astra_replacer_evict(astra_replacer *replacer,
                          astra_buffer_frame *frames,
                          uint32 frame_count,
                          uint64 *out_examined,
                          uint32 *out_frame_index);

/** Returns the frame index the clock will examine next. Pure. */
uint32 astra_replacer_hand(const astra_replacer *replacer);

/*
 * ---------------------------------------------------------------------------
 * buffer_pool.c - the page table
 * ---------------------------------------------------------------------------
 *
 * Every function here is called with the pool latch held and does not block, does not
 * allocate, and does not touch the Disk Manager. The `const` on `frames` in the lookup
 * is deliberate: a lookup must not be able to change what it is looking for.
 */

/** Mixes a page identifier into a well distributed 64-bit value. */
uint64 astra_page_hash(page_id_t page_id);

/** Initialises an empty table with at least `frame_count * 2` buckets. */
astra_status astra_page_table_init(astra_page_table *table, uint32 frame_count);

/** Releases the bucket array. Entries are not released; the frames own themselves. */
void astra_page_table_destroy(astra_page_table *table);

/**
 * Returns the index of the frame holding `page_id`, or ASTRA_FRAME_NONE.
 *
 * Chain walking, so the answer is a frame index and the caller can get the page from
 * it without the table knowing anything about pages.
 */
uint32 astra_page_table_find(const astra_page_table *table,
                             const astra_buffer_frame *frames,
                             page_id_t page_id);

/**
 * Links the frame already bound to `page_id` at `frame_index` into the table.
 *
 * Pushes at the head of the bucket's chain, so an insert touches exactly one frame and
 * cannot fail once it has started.
 *
 * Two things are checked before anything is changed, and both are caller bugs rather
 * than conditions to recover from:
 *   - `page_id` is not already in the table. The pool only inserts after establishing
 *     that the page is not resident, under one latch hold.
 *   - `frames[frame_index].page_id == page_id`. The frame has to be bound before it is
 *     linked, because the identifier is the key every later lookup compares against, and
 *     a frame linked under a key it does not carry is a bucket that points at a frame
 *     holding a different page.
 *
 * Returns true on success, false if either check fails, in which case nothing was
 * changed.
 */
bool astra_page_table_insert(astra_page_table *table,
                             astra_buffer_frame *frames,
                             page_id_t page_id,
                             uint32 frame_index);

/** Unlinks whichever frame holds `page_id`. Returns false if it held none. */
bool astra_page_table_remove(astra_page_table *table,
                             astra_buffer_frame *frames,
                             page_id_t page_id);

/** Returns the number of identifiers in the table. */
uint64 astra_page_table_count(const astra_page_table *table);

/** Initialises an empty retired set. Never fails; a zeroed set is a valid empty one. */
void astra_retired_set_init(astra_retired_set *set);

/** Releases the retired set's storage. NULL is allowed. */
void astra_retired_set_destroy(astra_retired_set *set);

/** Returns true when `page_id` has been retired. */
bool astra_retired_set_contains(const astra_retired_set *set, page_id_t page_id);

/**
 * Records `page_id` as retired.
 *
 * Insertion keeps the array sorted so that contains is a binary search. Retiring an
 * identifier that is already retired is not an error: the pool's public delete
 * accepts a page it does not hold, and deleting twice must be as harmless as deleting
 * a page that was already evicted.
 */
astra_status astra_retired_set_add(astra_retired_set *set, page_id_t page_id);

#endif /* ASTRA_STORAGE_BUFFER_BUFFER_INTERNAL_H */
