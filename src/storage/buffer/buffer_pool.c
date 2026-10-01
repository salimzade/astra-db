/*
 * AstraDB storage: the Buffer Pool's public entry points.
 *
 * The whole file is one latch and two rules about it, and those two rules are what make
 * the rest readable. They are stated at the top of buffer_internal.h and repeated only
 * where they are load-bearing:
 *
 *   1. The latch is never held across a call into the Disk Manager.
 *   2. Anything the latch protects that a thread will use outside the latch is pinned
 *      first and released last.
 *
 * Nearly every function below therefore has the same three beats: take the latch and
 * find or reserve a frame, drop the latch and do the I/O, take the latch again and
 * publish the result. The waiting in the middle is a loop on a condition variable rather
 * than a sleep, and each loop re-checks its own predicate, because a wakeup says that
 * something changed and not that this caller's condition became true.
 *
 * ---------------------------------------------------------------------------
 * The one thing worth reading twice: evicting a dirty frame
 * ---------------------------------------------------------------------------
 * A dirty victim cannot simply be overwritten. Its bytes are the caller's uncommitted
 * work, and the only thing that can move them to a durable place is the Disk Manager,
 * which must not be called with the latch held. So eviction is a four-step dance in
 * pool_choose_frame, and each step exists to close a hole the previous one opened:
 *
 *   1. Choose a frame that is not in motion. The replacer skips any frame that is pinned,
 *      loading or being written, so by construction it cannot hand back a frame whose
 *      bytes are on their way to or from the disk.
 *   2. Unlink the victim from the page table *before* writing it, and pin it as it is
 *      unlinked. Both halves matter and they answer different objections. The unlink
 *      means no lookup can find the frame during the write, so no fetch can hand its
 *      buffer to a caller who is about to have it overwritten; the pin means the clock,
 *      which reads pin counts rather than the page table, cannot select the same frame
 *      again while this thread is still inside the write. Together they are what "this
 *      frame is mine until I say otherwise" means.
 *   3. Write the dirty victim, with the latch dropped. The frame keeps its own page_id
 *      throughout - only the table entry is gone, not the page - because that identifier
 *      is the one the Disk Manager checks the buffer against.
 *   4. Bind the frame to the page that was wanted and publish it once the read that
 *      follows has returned.
 *
 * Unlinking first is the non-obvious half. The alternative - leaving the old key in place
 * during the write and making fetchers wait on `write_latched` - sounds safer, and it is
 * worse: `pool_write_frame` releases the latch, so for the whole duration of the write a
 * frame is reachable *and* about to be claimed for a different page. A fetch that pins it
 * in that window has its pin count overwritten by astra_frame_claim, so its caller is
 * left holding a buffer that now belongs to somebody else and an unpin that reports a
 * page it was just given as missing. No assertion fires, no file is corrupted, and the
 * failure surfaces in a different thread from the one that caused it. Being unfindable
 * and pinned is the only pair of properties that leaves no window at all.
 *
 * A write that fails stops the eviction. The frame is left holding its old page, still
 * dirty, and it is linked back into the table - so the very next fetch retries it and
 * reports the failure again, and the caller's data is still there rather than silently
 * gone. Failing to re-link would be worse than a failed fetch: the page would sit in a
 * frame nothing can find while the next read of it loaded a second copy from the disk.
 */
#include "storage/buffer/buffer_internal.h"

#include <stdatomic.h>
#include <stdio.h>

/*
 * ---------------------------------------------------------------------------
 * The one pool per Disk Manager rule
 * ---------------------------------------------------------------------------
 *
 * The public contract says a Disk Manager must not back two pools, and this is where
 * that is enforced. It is enforced here rather than in the Disk Manager because the
 * constraint is about the *pool*: a second pool over the same handle would have its own
 * page table and its own frames, so the two would each believe they owned the handle's
 * dirty state and each would evict by overwriting buffers the other was writing from.
 * The Disk Manager has no reason to know.
 *
 * A lock-free array of borrowed-handle addresses, probed with a compare-exchange per
 * slot. Not a mutex, and the reason is worth stating: a mutex here would need to exist
 * before anything can lock it, and a lazily-initialised global mutex needs its own
 * initialisation race, which is the sort of thing that works in every test and fails
 * once. C11 atomics have no such problem, because an all-zero static `_Atomic` object
 * is already fully initialised - there is no constructor to run and no order to get
 * wrong, at process start or during teardown.
 *
 * The registry holds the *address* of the borrowed handle, which is the whole identity
 * available and is sufficient: an entry is removed when the pool is destroyed, and a
 * pool that is never destroyed is a pool whose Disk Manager must not be reused, because
 * that pool could still read from and write to whatever now owns the address. Refusing
 * the new pool is the safe answer there rather than a nuisance.
 *
 * Sixty-four slots is a compile-time constant and not a limit that can be reached by
 * accident: a process that opened more than sixty-four databases at once, before
 * configuring a buffer size for each, has a design problem rather than a pool problem.
 * The failure is reported as ASTRA_ERR_ALREADY_EXISTS rather than a distinct
 * out-of-tables status, because "this handle cannot have a pool right now" is the same
 * thing to the caller either way.
 */
#define ASTRA_POOL_REGISTRY_SLOTS 64u

static _Atomic(astra_disk_manager *) g_pool_owners[ASTRA_POOL_REGISTRY_SLOTS];

/**
 * Claims `disk` for a pool. Returns false if it is already claimed, or if the registry
 * is full.
 *
 * One pass. For each slot, either the compare-exchange finds it free and takes it, or it
 * fails because the slot is occupied, and then the value is read to see whether the
 * occupant is the handle being claimed. That handles two threads racing for the *same*
 * handle without a retry: they contend for the same slot, one wins, and the loser reads
 * the winner's write and reports the collision. They also contend for the same slot when
 * racing for *different* handles, which is the only contention here, and one of them
 * simply moves on to the next slot.
 */
static bool pool_registry_claim(astra_disk_manager *disk)
{
    astra_disk_manager *empty = NULL;
    uint32 i;

    for (i = 0u; i < ASTRA_POOL_REGISTRY_SLOTS; ++i) {
        if (atomic_compare_exchange_strong_explicit(&g_pool_owners[i], &empty, disk,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            return true;
        }

        /* The exchange failed, so the slot holds something. `empty` was overwritten with
         * the value that was there, which is the one read needed to distinguish "someone
         * else's handle" from "this handle". */
        if (empty == disk) {
            return false;
        }

        empty = NULL;
    }

    return false;
}

static void pool_registry_release(astra_disk_manager *disk)
{
    uint32 i;

    for (i = 0u; i < ASTRA_POOL_REGISTRY_SLOTS; ++i) {
        /* Only this exact value is cleared, so a release can never unregister a pool that
         * a later pool over a recycled address has already claimed. */
        if (atomic_load_explicit(&g_pool_owners[i], memory_order_acquire) == disk) {
            atomic_store_explicit(&g_pool_owners[i], NULL, memory_order_release);
            return;
        }
    }
}

/*
 * ---------------------------------------------------------------------------
 * Latch helpers
 * ---------------------------------------------------------------------------
 *
 * The public query functions take a `const astra_buffer_pool *`, because a caller
 * inspecting a pool's counters must not be able to change it. That const does not mean a
 * counter needs no synchronisation, and the counters are latch-guarded, so those four
 * functions cast the const away through `uintptr_t` rather than with a plain C cast. The
 * cast is the point: it says the *caller's* promise is kept even though the
 * *implementation* is allowed to write to the latch. Casting away const directly would
 * hide that distinction, and would be a lie the moment anybody added a write to one of
 * those functions.
 */

/** Returns the frame index holding `page_id`, or ASTRA_FRAME_NONE. Latch held. */
static uint32 pool_find(const astra_buffer_pool *pool, page_id_t page_id)
{
    return astra_page_table_find(&pool->table, pool->frames.frames, page_id);
}

/**
 * Waits until the frame at `index` is not being loaded into and not being written.
 *
 * One predicate, two flags, because there is only one useful thing to do about either:
 * wait. The two are checked together rather than in sequence so that a waiter cannot sleep
 * through a load that finished just before a write started.
 *
 * Latch held on entry and on exit.
 */
static void pool_wait_idle(astra_buffer_pool *pool, uint32 index)
{
    while (pool->frames.frames[index].is_loading || pool->frames.frames[index].write_latched) {
        astra_latch_wait(&pool->latch);
    }
}

/**
 * Pins a frame and then waits for it to stop moving.
 *
 * Pin first, wait second, and the order is the whole point.
 *
 * Waiting alone is not enough, because astra_latch_wait releases the latch: between the
 * moment a caller observes a frame and the moment it acts on that observation, any other
 * thread may evict the frame and load a different page into it. A caller that looked up a
 * frame, waited for it to be idle, and then acted on what it found would act on whatever
 * page now occupies that index - or, if the frame had just been released, on a frame whose
 * page identifier names nothing, which the Disk Manager refuses with NOT_FOUND. Neither
 * failure is a corrupted file, and both are baffling when they happen, because the code
 * that produced them is a correct-looking sequence of correct steps.
 *
 * A pin is the pool's own statement that a frame may not be reused, so taking it before the
 * wait closes the window: the frame that becomes idle is the frame the caller decided to
 * use. This is not a refinement. The fetch hit path is a hit precisely because nobody is
 * pinning the frame, so an unpinned wait there races the replacer and returns the caller a
 * buffer belonging to a different page under the right page's name.
 *
 * The pin belongs to the caller, which must release it with pool_release_pin. Callers that
 * go on to write the frame hand a second pin to pool_write_frame, and the two are
 * independent; nothing here releases the caller's pin, so the unpin that follows must.
 *
 * Latch held on entry and on exit.
 */
static void pool_pin_and_wait(astra_buffer_pool *pool, uint32 index)
{
    ++pool->frames.frames[index].pin_count;
    pool_wait_idle(pool, index);
}

/**
 * Drops one pin, and wakes anybody waiting for the frame to become evictable.
 *
 * Every pin the pool gives itself goes back through here, so that "the count reached zero,
 * and someone may be sweeping behind me" is stated once. The broadcast is inside the
 * decrement rather than after it, because a waiter re-checks its predicate under the latch
 * and a broadcast issued after the latch has been released could be missed entirely.
 *
 * The pin count is not checked. Every caller of this function holds a pin it was given, and
 * a caller that does not is a caller that has already lost track of its own frame; the
 * public unpin is where that mistake is reported, and it is reported to the caller rather
 * than absorbed here.
 *
 * Latch held on entry and on exit.
 */
static void pool_release_pin(astra_buffer_pool *pool, uint32 index)
{
    if (pool->frames.frames[index].pin_count > 0u) {
        --pool->frames.frames[index].pin_count;
    }
    if (pool->frames.frames[index].pin_count == 0u) {
        astra_latch_broadcast(&pool->latch);
    }
}

/*
 * ---------------------------------------------------------------------------
 * Writing a frame
 * ---------------------------------------------------------------------------
 */

/**
 * Writes one frame's bytes to disk.
 *
 * The latch must be held on entry, and is held again on return. Between the two it is
 * dropped, so the pool's metadata is available for the whole duration of the write.
 *
 * The frame is pinned and write-latched first, which is what makes that safe: the pin
 * stops the clock choosing this frame for an eviction of its own, and the write latch
 * stops a fetch handing the buffer to a caller while the Disk Manager is reading it.
 * Both are undone before the latch is released, and the broadcast happens with the latch
 * held, because a waiter re-checks its predicate inside the latch and would otherwise be
 * able to sleep straight through the broadcast it was waiting for.
 *
 * `page_id` is passed rather than read from the frame, and the header page is refused
 * here rather than by each caller. Both are about having one place that decides what may
 * be written: `page_id` is the identity the caller was asked to write, which is not
 * necessarily what the frame now holds once a delete has unlinked it, and page 0 is
 * refused at the choke point so that no future caller can forget to. Every caller also
 * checks, because reporting the problem early is friendlier than reporting it from
 * underneath; this is the backstop, not the check.
 */
static astra_status pool_write_frame(astra_buffer_pool *pool, uint32 index, page_id_t page_id)
{
    astra_status status;

    if (page_id == 0u) {
        /* Page 0 is the file's magic number and checksum. The Disk Manager refuses it and
         * would be right to; refusing here means the refusal is this module's own
         * promise rather than an accident of a collaborator's implementation. */
        return ASTRA_ERR_INVALID_STATE;
    }

    pool->frames.frames[index].write_latched = true;
    ++pool->frames.frames[index].pin_count;

    astra_latch_unlock(&pool->latch);
    status = astra_disk_manager_write_page(pool->disk, &pool->frames.frames[index].page);
    astra_latch_lock(&pool->latch);

    pool->frames.frames[index].write_latched = false;
    pool_release_pin(pool, index);

    if (status == ASTRA_OK) {
        /* Cleared only on success. A failed write must leave the page dirty, so that the
         * next flush or the next eviction tries again instead of the modification being
         * forgotten by a flag that was cleared before the bytes had anywhere to go. */
        pool->frames.frames[index].is_dirty = false;
    }

    astra_latch_broadcast(&pool->latch);
    return status;
}

/*
 * ---------------------------------------------------------------------------
 * Choosing a frame
 * ---------------------------------------------------------------------------
 */

/**
 * Finds a frame to load a page into, flushing a dirty victim if there is one.
 *
 * Sources, in order:
 *
 *   1. A frame that has never held a page, handed out in index order so a pool's first
 *      fetch storm spreads across the array instead of hammering frame 0.
 *   2. The clock's victim.
 *
 * A cold frame cannot need a flush and cannot be busy, because "cold" means it has never
 * been claimed and so has never been loaded into or written from. Only a victim goes
 * through the eviction dance described at the top of this file - and a victim is, by
 * construction, a frame the replacer has already established is neither pinned nor in
 * motion, so this function does not have to loop looking for a second candidate and
 * cannot spin.
 *
 * On success the frame is pinned, and is *not* in the page table under the new page's
 * key. That is what makes the gap between reserving and publishing invisible: a
 * concurrent fetch of the same identifier does not find this frame, so it neither
 * redirects to a frame whose contents do not exist yet nor waits on a reservation it
 * cannot know about. It takes a frame of its own and reads the same bytes, and whichever
 * of the two publishes first wins while the other discards its frame and retries. Two
 * concurrent fetches of one cold page therefore cost two reads and no blocking, which is
 * the right trade: the reads are the thing a disk cache exists to absorb, and the
 * alternative - an in-flight identifier set, consulted before a frame is even reserved -
 * would add a table lookup and a wakeup to every miss, including the common single-
 * threaded one.
 *
 * `page_id` may be ASTRA_PAGE_ID_INVALID, which is how the new-page path calls this: the
 * identifier does not exist until the file has been extended, and a frame bound to
 * nothing is invisible and untouchable, which is exactly what a reservation wants to be.
 *
 * Latch held on entry and on exit. `out_cold` reports which of the two sources was used,
 * because the caller has to be able to undo the reservation correctly if the load or the
 * allocation that follows it fails - see pool_abandon_frame, which is where the
 * difference matters.
 *
 * Returns ASTRA_OK, ASTRA_ERR_INVALID_STATE when every frame is pinned, or the Disk
 * Manager's status if a dirty victim could not be written.
 */
static astra_status pool_choose_frame(astra_buffer_pool *pool,
                                      page_id_t page_id,
                                      uint32 *out_index,
                                      bool *out_cold)
{
    astra_status status;
    uint32 index;
    uint64 examined;

    if (pool->frames.fresh_next < pool->frames.count) {
        index = pool->frames.fresh_next;
        ++pool->frames.fresh_next;
        astra_frame_claim(&pool->frames.frames[index], page_id);
        ++pool->frames.used;
        *out_index = index;
        *out_cold = true;
        return ASTRA_OK;
    }

    if (!astra_replacer_evict(&pool->replacer, pool->frames.frames, pool->frames.count,
                              &examined, &index)) {
        /*
         * Two complete sweeps and nothing evictable, which - see the replacer - can only
         * mean every frame is pinned. The caller is holding more pages than the pool has
         * frames. It is not waited for and it is not stolen from: both would make the
         * pool's behaviour depend on a thread the caller does not control.
         */
        return ASTRA_ERR_INVALID_STATE;
    }
    pool->replacement_count += examined;

    /*
     * The old key comes out of the page table, and it comes out **before** the frame is
     * written, not after.
     *
     * The order is the whole correctness of this function, and getting it wrong is silent.
     * pool_write_frame drops the latch to do the write, and during that window a frame
     * that is still filed under its old key is a frame a concurrent fetch can find, pin,
     * and start using. The eviction then comes back, claims the frame for a different
     * page, and astra_frame_claim overwrites the pin count with 1 - so the fetch's pin is
     * erased rather than released. Its caller is holding a buffer that now belongs to
     * somebody else, its unpin drives the count below zero, and the pool reports the page
     * as missing to a caller that was handed it moments earlier. No assertion fires, no
     * file is corrupted, and the failure appears in a different thread from the one that
     * caused it.
     *
     * Unlinking first closes the window from the other side: for the whole of the write
     * the frame is pinned (so the clock cannot choose it) and unfindable (so no lookup
     * can name it), and those two together are what "this frame is mine until I say
     * otherwise" means. A frame that is dirty and reachable is a frame two threads are
     * about to disagree about.
     */
    if (pool->frames.frames[index].in_table) {
        page_id_t old_page_id = pool->frames.frames[index].page_id;

        (void)astra_page_table_remove(&pool->table, pool->frames.frames, old_page_id);

        /*
         * The old occupant stops being resident, and the new one starts. Written as a
         * pair rather than as nothing at all because the two are not the same operation:
         * incrementing without decrementing makes `used` count evictions instead of
         * frames, so it climbs past the pool's own size and a caller watching it has no
         * way to tell a busy pool from a broken counter.
         */
        --pool->frames.used;

        /*
         * Pinned here, at the moment the frame is unlinked, and this is the second half
         * of the invariant the unlink alone does not establish.
         *
         * Unlinking makes the frame unfindable, but the clock does not look in the page
         * table - it looks at pin counts. Between this unlink and the claim below, the
         * frame is unpinned, not loading and not being written, which is exactly the
         * state astra_replacer_evict reports as a valid victim. pool_write_frame drops
         * the latch to do the write, so a concurrent eviction really can sweep past and
         * re-select this same frame. It would then remove the old key a second time,
         * underflow `used`, and claim the frame for its own page while this thread is
         * still inside the write - leaving one frame chained under two page identifiers,
         * which is the corruption this whole dance is meant to prevent.
         *
         * The pin is not released on the paths below: astra_frame_claim overwrites the
         * count with 1, which is the reservation this thread is about to hand back to its
         * caller. So the pin taken here *becomes* the caller's pin rather than being an
         * extra one, and the only path that has to undo it by hand is the failed-write
         * path below.
         */
        ++pool->frames.frames[index].pin_count;

        /* The dirty victim is written, now that nothing can reach it. The frame keeps its
         * own page_id throughout, because that is the identity the Disk Manager checks
         * the buffer against; only the *table entry* is gone, not the page. */
        if (pool->frames.frames[index].is_dirty) {
            status = pool_write_frame(pool, index, old_page_id);
            if (status != ASTRA_OK) {
                /*
                 * The eviction is abandoned, not completed, and the key goes back in. The
                 * frame still holds its page and is still dirty, so the data is intact; it
                 * is simply invisible for as long as it takes to put it back, which is the
                 * only window in which a page in memory is not reachable by name. Refusing
                 * to re-link it would be a worse answer than a failed fetch: the page would
                 * be stranded in a frame nothing can find, and the next fetch would load it
                 * from disk again, over the top of the copy that is still dirty here.
                 *
                 * The frame is re-linked with the reference bit clear, as the clock left it,
                 * so the failure resurfaces at the front of the next sweep rather than being
                 * swallowed until a page of other evictions have gone past.
                 */
                (void)astra_page_table_insert(&pool->table, pool->frames.frames,
                                              old_page_id, index);
                ++pool->frames.used;
                pool_release_pin(pool, index);
                return status;
            }
        }
    }

    /* The frame is bound to its new page. It is still not in the table - the caller
     * inserts it once the read has completed, which is what makes a reservation invisible
     * to a concurrent fetch of the same identifier. */
    astra_frame_claim(&pool->frames.frames[index], page_id);
    ++pool->frames.used;

    *out_index = index;
    *out_cold = false;
    return ASTRA_OK;
}

/**
 * Undoes a reservation that never reached the point of being published.
 *
 * A frame that was bound to a key but never linked is invisible, and invisible frames
 * leak: they are pinned, so the clock can never choose them, and they are not in the
 * table, so no lookup can release them. Every failure path after pool_choose_frame
 * therefore has to come through here, and there are two of them - a read that failed, and
 * an allocation that failed - which is why this is one function rather than two copies
 * of the same three lines.
 *
 * A frame that came from the cold cursor is *not* handed back to it, and that is the whole
 * subtlety of this function.
 *
 * The obvious thing to do is to decrement `fresh_next` here, so that a pool which fails one
 * load does not end up with a permanently shorter cold run. Doing that is a use-after-free
 * of the cursor's invariant, and it corrupts the pool in a way nothing reports. The cursor
 * and the clock draw from the same frame array, and the only thing that keeps them from
 * overlapping is that `fresh_next` only ever moves forwards: the cursor owns the frames at
 * or above it, the clock owns everything below, and the clock is reached only once the
 * cursor is exhausted.
 *
 * Give a frame back and that ownership is no longer exclusive. The frame is released here -
 * pin zero, no page, no reference bit - which makes it indistinguishable from a frame the
 * clock is entitled to take, and the very next sweep can take it and load a real page into
 * it. The cursor, meanwhile, still points at it. The next cold reservation then claims a
 * frame that is resident and pinned by another thread: astra_frame_claim overwrites the pin
 * count with 1, erasing the other thread's pin, and binds a second page to the frame while
 * its first page is still filed against it in the page table. The holder of the erased pin
 * finishes, finds a pin count of zero where it left one, and its unpin either reports a
 * double unpin that never happened or drives the count below zero.
 *
 * Losing one cold slot per failed load is the price, and it is a price worth paying: a
 * failed load is rare, the slot is picked up by the clock on the next sweep like any other
 * free frame, and a pool that is failing loads is not one whose first-touch spreading
 * behaviour matters.
 */
static void pool_abandon_frame(astra_buffer_pool *pool, uint32 index, bool was_cold)
{
    (void)was_cold;

    astra_frame_release(&pool->frames.frames[index]);
    --pool->frames.used;

    astra_latch_broadcast(&pool->latch);
}

/*
 * ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

astra_status astra_buffer_pool_create(const astra_config *cfg,
                                      astra_disk_manager *disk,
                                      astra_buffer_pool **out_pool)
{
    astra_buffer_pool *pool;
    astra_status status;
    uint32 page_size;

    if (cfg == NULL || disk == NULL || out_pool == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    *out_pool = NULL;

    /*
     * The whole configuration is validated, not just the pool size. A pool built from a
     * configuration the rest of the process would reject is a pool whose page size
     * disagrees with something else, and finding that out here is free.
     */
    if (astra_config_validate(cfg) != ASTRA_OK) {
        return ASTRA_ERR_UNSUPPORTED;
    }

    page_size = astra_disk_manager_page_size(disk);
    if (page_size == 0u) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (cfg->page_size != page_size) {
        /*
         * Refused rather than reconciled. The Disk Manager is the authority on the stride
         * it reads the file at, so honouring `cfg` would mean trusting a configuration
         * that is demonstrably wrong about the file in front of it, and honouring the
         * manager would mean silently ignoring the caller. Neither is a repair, and both
         * end in a database that fails to open later.
         */
        return ASTRA_ERR_UNSUPPORTED;
    }

    /*
     * Checked before the allocation rather than after it, because the multiplication is
     * the thing that can overflow. On a 32-bit build a million frames of 16 KiB is
     * sixteen gigabytes, which as a size_t is a wrap rather than a refusal, and a
     * request for a wrapped size is a heap overflow in most allocators rather than an
     * error.
     */
    if ((size_t)cfg->buffer_pool_pages > SIZE_MAX / (size_t)page_size) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    if (!pool_registry_claim(disk)) {
        return ASTRA_ERR_ALREADY_EXISTS;
    }

    pool = astra_alloc_zeroed(1u, sizeof *pool);
    if (pool == NULL) {
        pool_registry_release(disk);
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    if (astra_latch_init(&pool->latch) != ASTRA_OK) {
        astra_dealloc(pool);
        pool_registry_release(disk);
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    /*
     * `fresh_next` starts at 0, which is the next frame never used, and the clock's hand
     * starts at 0 too. The two cursors deliberately begin in the same place: the hand
     * sweeps past the frames that are in use before it finds one to take, and starting
     * it anywhere else just makes the first eviction slower for no benefit.
     */
    status = astra_frame_table_init(&pool->frames, cfg->buffer_pool_pages, page_size);
    if (status != ASTRA_OK) {
        astra_latch_destroy(&pool->latch);
        astra_dealloc(pool);
        pool_registry_release(disk);
        return status;
    }

    status = astra_page_table_init(&pool->table, cfg->buffer_pool_pages);
    if (status != ASTRA_OK) {
        astra_frame_table_destroy(&pool->frames);
        astra_latch_destroy(&pool->latch);
        astra_dealloc(pool);
        pool_registry_release(disk);
        return status;
    }

    astra_retired_set_init(&pool->retired);
    pool->replacer.hand = 0u;
    pool->disk = disk;
    pool->page_size = page_size;

    *out_pool = pool;
    return ASTRA_OK;
}

/**
 * How much of the pool a write-back sweep covers, and what it does about a failure.
 *
 * The two callers genuinely want different things and collapsing them would have made one
 * of them lie. Destroy is on its way out and owes the caller every dirty byte it is
 * holding, so it keeps going past a failure and reports the first one at the end.
 * flush_all made a promise about the whole pool, so it stops at the first failure and says
 * so: pages already written are on disk and the rest are not, and a caller that wants the
 * rest has to know that to ask.
 */
typedef enum pool_flush_mode {
    /** Write the dirty frames, keep going after a failure, report the first one. */
    POOL_FLUSH_DIRTY,

    /** Write every resident frame, stop at the first failure. */
    POOL_FLUSH_EVERY
} pool_flush_mode;

/**
 * Writes resident frames, by frame index, and reports what happened.
 *
 * Shared by flush_all and destroy. It visits frames in index order - sequential access to
 * the frame array, and neither caller promises an ordering - and drops the latch for each
 * write, so a slow disk does not stop the pool being read.
 *
 * The header page is handled once, up front, rather than as the sweep reaches it. Page 0 is
 * never written: the Disk Manager refuses it, because it is the file's magic number and
 * checksum. A clean page 0 is simply skipped, and a *dirty* page 0 fails the whole call
 * before any byte is written, because a dirty header means a caller unpinned page 0 with
 * `dirty = true` and the pool is in a state it cannot honestly report as flushed. Checking
 * first means the file is left exactly as it was rather than partly written.
 *
 * Latch held on entry and on exit.
 */
static astra_status pool_write_resident(astra_buffer_pool *pool, pool_flush_mode mode)
{
    astra_status result = ASTRA_OK;
    astra_status status;
    uint32 index;

    for (index = 0u; index < pool->frames.count; ++index) {
        if (pool->frames.frames[index].in_table && pool->frames.frames[index].is_dirty
            && pool->frames.frames[index].page_id == 0u) {
            return ASTRA_ERR_INVALID_STATE;
        }
    }

    for (index = 0u; index < pool->frames.count; ++index) {
        page_id_t page_id;
        bool wanted_dirty = pool->frames.frames[index].is_dirty;

        if (!pool->frames.frames[index].in_table
            || pool->frames.frames[index].page_id == 0u) {
            continue;
        }
        if (mode == POOL_FLUSH_DIRTY && !wanted_dirty) {
            continue;
        }

        pool_pin_and_wait(pool, index);

        /*
         * Re-checked, not assumed. Two things can have changed while the latch was released
         * inside the wait: the frame may have stopped being dirty because another thread
         * flushed it, and - had the wait not pinned it - the frame may have been evicted and
         * given a different page. The pin makes the second impossible, and the re-read
         * handles the first. Re-reading the frame rather than the cached decision is what
         * keeps the two consistent, and it is also why `page_id` is read *after* the wait
         * rather than before: a decision and the page it was made about have to be read in
         * the same instant.
         */
        if (!pool->frames.frames[index].in_table
            || (mode == POOL_FLUSH_DIRTY && !pool->frames.frames[index].is_dirty)) {
            pool_release_pin(pool, index);
            continue;
        }

        page_id = pool->frames.frames[index].page_id;
        status = pool_write_frame(pool, index, page_id);
        pool_release_pin(pool, index);

        if (status != ASTRA_OK) {
            if (mode == POOL_FLUSH_EVERY) {
                return status;
            }
            if (result == ASTRA_OK) {
                result = status;
            }
        }
    }

    return result;
}

astra_status astra_buffer_pool_destroy(astra_buffer_pool *pool)
{
    astra_status result;
    astra_disk_manager *disk;
    uint32 index;
    uint64 pins;
    uint64 dirty;

    if (pool == NULL) {
        return ASTRA_OK;
    }

    /*
     * The latch is taken once, here, and held until the pool's memory is gone. Two of the
     * three things below need it: the scan reads the frames, and pool_write_resident
     * documents that it must be called with the latch held, because it calls
     * pool_write_frame - which drops and retakes the latch around every write. Calling it
     * unlocked would unlock a latch that was never locked, which is undefined rather than
     * merely wrong, and on a counting latch it corrupts the count for every later user.
     *
     * The header defines destroying a pool that other threads are still using as
     * undefined, so the latch is not making that safe - it is making the defined case work.
     */
    astra_latch_lock(&pool->latch);

    /*
     * Pinned pages at this point are a caller mistake, and a loud one: they mean threads
     * are still inside the pool, which the public header defines as undefined. The pages
     * are written anyway, because silently discarding a modification is the worse of the
     * two bad outcomes, and the count is reported so the caller can find the thread.
     */
    pins = 0u;
    dirty = 0u;
    for (index = 0u; index < pool->frames.count; ++index) {
        pins += pool->frames.frames[index].pin_count;
        if (pool->frames.frames[index].is_dirty) {
            ++dirty;
        }
    }

    if (pins > 0u) {
        ASTRA_LOG_WARN(ASTRA_SUBSYSTEM_BUFFER,
                       "pool destroyed with %llu pins and %llu dirty pages still held; "
                       "pages were flushed but the threads holding them are using "
                       "memory that has just been freed",
                       (unsigned long long)pins, (unsigned long long)dirty);
    }

    result = pool_write_resident(pool, POOL_FLUSH_DIRTY);

    /* Durability is part of closing a pool, not a separate decision to remember. */
    if (result == ASTRA_OK) {
        result = astra_disk_manager_sync(pool->disk);
    }

    disk = pool->disk;

    astra_latch_unlock(&pool->latch);

    astra_retired_set_destroy(&pool->retired);
    astra_page_table_destroy(&pool->table);
    astra_frame_table_destroy(&pool->frames);

    astra_latch_destroy(&pool->latch);
    astra_dealloc(pool);

    /*
     * Released last, after the pool's own memory is gone. A Disk Manager that outlives
     * its pool can be given a new one, and that is the only way the registry's
     * address-keyed identity stays meaningful.
     */
    pool_registry_release(disk);

    return result;
}

/*
 * ---------------------------------------------------------------------------
 * Reading pages
 * ---------------------------------------------------------------------------
 */

astra_status astra_buffer_pool_fetch_page(astra_buffer_pool *pool,
                                          page_id_t page_id,
                                          astra_page **out_page)
{
    astra_status status;
    uint32 index;
    bool cold = false;

    if (pool == NULL || out_page == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    *out_page = NULL;

    /*
     * ASTRA_PAGE_ID_INVALID is reported as "not found" rather than as a bad argument. It
     * is a value a stale reference plausibly holds, and the right answer to "this
     * identifier names nothing" and to "you passed nonsense" is the same action: do not
     * try to cache it.
     */
    if (page_id == ASTRA_PAGE_ID_INVALID) {
        return ASTRA_ERR_NOT_FOUND;
    }

    astra_latch_lock(&pool->latch);

    if (astra_retired_set_contains(&pool->retired, page_id)) {
        astra_latch_unlock(&pool->latch);
        return ASTRA_ERR_NOT_FOUND;
    }

    index = pool_find(pool, page_id);
    if (index != ASTRA_FRAME_NONE) {
        /* A hit. The frame may be mid-load or mid-write; either way the buffer is in
         * motion and handing it out would expose a half-finished read.
         *
         * The pin is taken before the wait, and that order is load-bearing rather than
         * tidy. A hit is by definition a frame nobody is pinning - the previous holder
         * unpinned it, which is how it became evictable - so waiting for it to settle
         * without holding it first is a race with the clock: the latch is released inside
         * the wait, the frame is evicted, a different page is loaded into it, and the
         * fetch then hands the caller a buffer full of somebody else's bytes while
         * reporting the page it was asked for. The caller's unpin is what finally notices,
         * by failing to find a page it was just given.
         */
        pool_pin_and_wait(pool, index);

        astra_replacer_record_access(&pool->frames.frames[index]);

        *out_page = &pool->frames.frames[index].page;
        astra_latch_unlock(&pool->latch);
        return ASTRA_OK;
    }

    /* A miss. */
    status = pool_choose_frame(pool, page_id, &index, &cold);
    if (status != ASTRA_OK) {
        astra_latch_unlock(&pool->latch);
        return status;
    }

    pool->frames.frames[index].is_loading = true;
    astra_latch_unlock(&pool->latch);

    /*
     * The read, with the latch dropped. The frame is pinned, so the clock cannot choose
     * it as its own victim, and its is_loading flag keeps it from being chosen as a
     * clean victim either.
     */
    status = astra_disk_manager_read_page(pool->disk, page_id, &pool->frames.frames[index].page);

    astra_latch_lock(&pool->latch);
    pool->frames.frames[index].is_loading = false;

    if (status != ASTRA_OK) {
        pool_abandon_frame(pool, index, cold);
        astra_latch_unlock(&pool->latch);
        return status;
    }

    /*
     * Publish. The frame has been holding this page's identifier since it was reserved but
     * was deliberately not linked while the read was in flight, so this is the instant the
     * page becomes findable - and until it does, no lookup, no flush, no delete and no pin
     * count can name it. Omitting the insert here leaves the page permanently invisible: it
     * would be served once and then never again, and the caller's unpin would fail with
     * NOT_FOUND, which is a confusing way to describe a missing line.
     *
     * Three outcomes, and all three have to be handled because the read happened with the
     * latch released:
     *
     *   - The page was retired by a delete while the read was in flight. The delete could
     *     not have seen this frame - it was not in the table - so it retired an identifier
     *     that was about to become resident again. The identifier is gone; the page read
     *     into a frame nothing will ever ask for is thrown away rather than resurrected.
     *   - Another thread loaded the same page concurrently and published it first. Both
     *     reads were correct and both frames hold identical bytes, but the pool keeps one
     *     key, so this frame is the duplicate and the published one is adopted. The caller
     *     still gets a page pointer, and it is the one that will still be resident next
     *     time, which is what makes two concurrent fetches of one page agree on identity.
     *   - Neither. This frame is published, and the pin the reservation took is the pin
     *     this call returns, which is why it is not incremented again here.
     */
    if (astra_retired_set_contains(&pool->retired, page_id)) {
        pool_abandon_frame(pool, index, cold);
        astra_latch_unlock(&pool->latch);
        return ASTRA_ERR_NOT_FOUND;
    }

    {
        uint32 winner = pool_find(pool, page_id);

        if (winner != ASTRA_FRAME_NONE) {
            pool_abandon_frame(pool, index, cold);

            /*
             * The published frame may still be mid-load itself, so this waits - and the
             * wait is pinned, because astra_latch_wait releases the latch and an unpinned
             * frame can be evicted inside that window. Pinning late is the bug this avoids:
             * the thread would come back from the wait holding a pin on whatever page had
             * since taken the frame, and its unpin of the page it asked for would find a
             * pin count of zero and report a double unpin that never happened.
             */
            pool_pin_and_wait(pool, winner);

            /* The pin the reservation took on `winner` is the pin this call returns, so it
             * is not incremented again. */
            astra_replacer_record_access(&pool->frames.frames[winner]);
            *out_page = &pool->frames.frames[winner].page;

            astra_latch_broadcast(&pool->latch);
            astra_latch_unlock(&pool->latch);
            return ASTRA_OK;
        }
    }

    if (!astra_page_table_insert(&pool->table, pool->frames.frames, page_id, index)) {
        /* The identifier is valid, the frame is bound to it, and the lookup above found
         * nothing, so this cannot fail. Reported rather than assumed away, because a frame
         * bound to a page that is not in the table would be invisible to every lookup and
         * would leak for the pool's lifetime. */
        pool_abandon_frame(pool, index, cold);
        astra_latch_unlock(&pool->latch);
        return ASTRA_ERR_INTERNAL;
    }

    astra_replacer_record_access(&pool->frames.frames[index]);
    *out_page = &pool->frames.frames[index].page;

    astra_latch_broadcast(&pool->latch);
    astra_latch_unlock(&pool->latch);
    return ASTRA_OK;
}

astra_status astra_buffer_pool_new_page(astra_buffer_pool *pool,
                                        astra_page **out_page,
                                        page_id_t *out_page_id)
{
    astra_status status;
    page_id_t page_id = ASTRA_PAGE_ID_INVALID;
    uint32 index;
    bool cold = false;

    if (pool == NULL || out_page == NULL || out_page_id == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    *out_page = NULL;
    *out_page_id = ASTRA_PAGE_ID_INVALID;

    /*
     * The frame is taken *before* the file is extended, so that a pool with nowhere to put
     * the page fails without having created one.
     *
     * The identifier does not exist yet, so the reservation is made against
     * ASTRA_PAGE_ID_INVALID: the frame is pinned, holds no page name, and is not in the
     * page table - and the page table's insert check refuses an invalid key, so it cannot
     * be filed under one. That leaves it invisible and untouchable: no lookup can name it
     * and the clock cannot choose it because it is pinned. Nothing can observe a pool in
     * this state.
     *
     * The eviction half is pool_choose_frame's, not a second copy of it. A new page needs a
     * frame exactly as much as a fetch does, so a dirty victim here has to be written for
     * the same reason and with the same unlink-then-pin ordering; a second implementation
     * would be free to disagree with the first about both.
     */
    astra_latch_lock(&pool->latch);

    status = pool_choose_frame(pool, ASTRA_PAGE_ID_INVALID, &index, &cold);
    if (status != ASTRA_OK) {
        astra_latch_unlock(&pool->latch);
        return status;
    }

    astra_latch_unlock(&pool->latch);

    /* The extend, with the latch dropped, into a frame nothing else can name. */
    status = astra_disk_manager_alloc_page(pool->disk, &page_id);
    if (status != ASTRA_OK) {
        astra_latch_lock(&pool->latch);
        pool_abandon_frame(pool, index, cold);
        astra_latch_unlock(&pool->latch);
        return status;
    }

    astra_latch_lock(&pool->latch);

    /*
     * Bind and publish. The frame's buffer was cleared by the claim and the Disk Manager
     * guarantees an allocated page reads back as zeros, so the buffer and the file agree
     * without either having been written.
     *
     * Marked dirty, which costs one redundant write of zeros in the common case and
     * prevents losing a caller's work in the case that matters: a page filled in and then
     * unpinned with `dirty = false`, or a process that dies before either. Erring towards
     * an extra write is the only asymmetry here that does not risk data.
     */
    astra_frame_claim(&pool->frames.frames[index], page_id);
    pool->frames.frames[index].is_dirty = true;
    pool->frames.frames[index].page.is_dirty = true;

    if (!astra_page_table_insert(&pool->table, pool->frames.frames, page_id, index)) {
        /* Cannot fail: the identifier was just allocated, so it is neither resident nor
         * retired, and the frame is bound to it. Reported rather than assumed away,
         * because a frame bound to a page that is not in the table would be invisible to
         * every lookup and would leak for the pool's lifetime. */
        pool_abandon_frame(pool, index, cold);
        astra_latch_unlock(&pool->latch);
        return ASTRA_ERR_INTERNAL;
    }

    astra_replacer_record_access(&pool->frames.frames[index]);

    *out_page = &pool->frames.frames[index].page;
    *out_page_id = page_id;

    astra_latch_broadcast(&pool->latch);
    astra_latch_unlock(&pool->latch);
    return ASTRA_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Releasing pages
 * ---------------------------------------------------------------------------
 */

astra_status astra_buffer_pool_unpin_page(astra_buffer_pool *pool,
                                          page_id_t page_id,
                                          bool dirty)
{
    uint32 index;

    if (pool == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (page_id == ASTRA_PAGE_ID_INVALID) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    astra_latch_lock(&pool->latch);

    index = pool_find(pool, page_id);
    if (index == ASTRA_FRAME_NONE) {
        /*
         * Not resident. The caller believes it holds a pin, and the reason it does not is
         * that the page was evicted - which is exactly what the pin was supposed to
         * prevent. Reporting it rather than treating it as a no-op is the interlock the
         * public header describes; a silent success would leave the caller working
         * through a pointer that has already been recycled.
         */
        astra_latch_unlock(&pool->latch);
        return ASTRA_ERR_NOT_FOUND;
    }

    if (pool->frames.frames[index].pin_count == 0u) {
        /* The double unpin. Reported rather than tolerated: a page in use with no pin is
         * one the next fetch is allowed to evict, which is the failure this API exists to
         * make impossible. */
        astra_latch_unlock(&pool->latch);
        return ASTRA_ERR_INVALID_STATE;
    }

    pool_release_pin(pool, index);

    if (dirty) {
        /*
         * Declared here, at the unpin, because the pool cannot tell a modification from
         * a read of the buffer. The page's own `is_dirty` is set too: the Disk Manager
         * clears it on a successful write, and the frame's copy and the buffer's copy
         * have to agree about whether the two differ.
         */
        pool->frames.frames[index].is_dirty = true;
        pool->frames.frames[index].page.is_dirty = true;
    }

    /*
     * Broadcast when the count reached zero, because that is the moment a frame becomes
     * evictable and a thread sweeping behind it may have just given up. The broadcast
     * happens with the latch held, so a waiter re-checks under the latch and cannot miss
     * it.
     */
    if (pool->frames.frames[index].pin_count == 0u) {
        astra_latch_broadcast(&pool->latch);
    }

    astra_latch_unlock(&pool->latch);
    return ASTRA_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Writing back
 * ---------------------------------------------------------------------------
 */

astra_status astra_buffer_pool_flush_page(astra_buffer_pool *pool, page_id_t page_id)
{
    astra_status status;
    uint32 index;

    if (pool == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (page_id == ASTRA_PAGE_ID_INVALID) {
        return ASTRA_ERR_NOT_FOUND;
    }

    astra_latch_lock(&pool->latch);

    index = pool_find(pool, page_id);
    if (index == ASTRA_FRAME_NONE) {
        /*
         * Not cached. Reading it in only to write it straight back is not an operation
         * anybody wants, and a caller who does is reaching for the Disk Manager, which is
         * where that behaviour belongs.
         */
        astra_latch_unlock(&pool->latch);
        return ASTRA_ERR_NOT_FOUND;
    }

    if (page_id == 0u) {
        /* Refused before the frame is pinned and latched, so a call that cannot succeed
         * costs nothing and reports the pool's reason rather than the Disk Manager's. */
        astra_latch_unlock(&pool->latch);
        return ASTRA_ERR_INVALID_STATE;
    }

    pool_pin_and_wait(pool, index);

    /* Clean or dirty, the page is written: the call is about this page, not its state.
     * pool_write_frame takes its own pin for the duration of the write, so this one is
     * dropped here. */
    status = pool_write_frame(pool, index, page_id);
    pool_release_pin(pool, index);

    astra_latch_unlock(&pool->latch);
    return status;
}

astra_status astra_buffer_pool_flush_all(astra_buffer_pool *pool)
{
    astra_status status;

    if (pool == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    /*
     * One sweep, not two. The obvious structure - write the dirty pages, then sweep again
     * to write the clean ones - writes every dirty page twice and splits one guarantee
     * across two passes. POOL_FLUSH_EVERY covers both in one walk: a clean page is written
     * too, because "flush everything" is a claim about durability and a clean page's bytes
     * on disk are exactly what it promised.
     */
    astra_latch_lock(&pool->latch);
    status = pool_write_resident(pool, POOL_FLUSH_EVERY);
    astra_latch_unlock(&pool->latch);

    if (status != ASTRA_OK) {
        return status;
    }

    /*
     * The sync is inside flush_all rather than left to the caller, because "flush
     * everything" is a call made when a caller stops caring, and the version that wrote
     * the pages and left them in the operating system's hands is the version nobody
     * remembered to follow with a sync.
     */
    return astra_disk_manager_sync(pool->disk);
}

/*
 * ---------------------------------------------------------------------------
 * Deleting
 * ---------------------------------------------------------------------------
 */

astra_status astra_buffer_pool_delete_page(astra_buffer_pool *pool, page_id_t page_id)
{
    astra_status status;
    uint32 index;

    if (pool == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (page_id == ASTRA_PAGE_ID_INVALID) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (page_id == 0u) {
        /* The header page belongs to the file. Deleting it would leave something that is
         * not a database, and the Disk Manager has no operation that could put one back. */
        return ASTRA_ERR_INVALID_STATE;
    }

    astra_latch_lock(&pool->latch);

    index = pool_find(pool, page_id);
    if (index != ASTRA_FRAME_NONE) {
        if (pool->frames.frames[index].pin_count > 0u) {
            /*
             * In use, and not waited for. A delete that blocked behind a thread's pin
             * would have its completion time controlled by a thread the caller may not
             * even know about, and a thread that never unpins would hang it forever. The
             * caller knows it pinned the page; it can drop the pin and try again.
             */
            astra_latch_unlock(&pool->latch);
            return ASTRA_ERR_INVALID_STATE;
        }
        if (pool->frames.frames[index].write_latched) {
            /* Being written. Dropping the frame now would pull the buffer out from under
             * a Disk Manager write in flight. */
            astra_latch_unlock(&pool->latch);
            return ASTRA_ERR_INVALID_STATE;
        }

        /*
         * The dirty state is discarded rather than written. A page being deleted is a page
         * whose contents are being abandoned, and writing bytes no identifier will ever
         * name again is the most expensive way to do nothing.
         */
        (void)astra_page_table_remove(&pool->table, pool->frames.frames, page_id);
        astra_frame_release(&pool->frames.frames[index]);
        --pool->frames.used;
    }

    /*
     * Retired after the frame is gone, not before, so that "a resident page is never
     * retired" is true at every instant rather than merely usually. A fetch cannot get
     * between the two because the latch is held across both. And if the retirement fails
     * for want of memory the pool is left in the state where the page is merely evicted
     * rather than gone - which is safe - rather than in the state where it is neither
     * resident nor retired, which would let a later fetch resurrect it. That is why the
     * order is this way round.
     */
    status = astra_retired_set_add(&pool->retired, page_id);
    if (status != ASTRA_OK) {
        astra_latch_unlock(&pool->latch);
        return status;
    }

    astra_latch_broadcast(&pool->latch);
    astra_latch_unlock(&pool->latch);
    return ASTRA_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Queries
 * ---------------------------------------------------------------------------
 */

uint32 astra_buffer_pool_capacity(const astra_buffer_pool *pool)
{
    if (pool == NULL) {
        return 0u;
    }
    /*
     * Fixed for the pool's life and published at construction, so this needs no latch.
     * Reading it without one is what lets a caller size an array before touching the pool
     * at all.
     */
    return pool->frames.count;
}

uint32 astra_buffer_pool_page_size(const astra_buffer_pool *pool)
{
    if (pool == NULL) {
        return 0u;
    }
    return pool->page_size;
}

astra_status astra_buffer_pool_used_frames(const astra_buffer_pool *pool, uint64 *out_used)
{
    if (pool == NULL || out_used == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    astra_latch_lock(&((astra_buffer_pool *)(uintptr_t)pool)->latch);
    *out_used = pool->frames.used;
    astra_latch_unlock(&((astra_buffer_pool *)(uintptr_t)pool)->latch);

    return ASTRA_OK;
}

astra_status astra_buffer_pool_dirty_frames(const astra_buffer_pool *pool, uint64 *out_dirty)
{
    astra_buffer_pool *mutable_pool = (astra_buffer_pool *)(uintptr_t)pool;
    uint64 count = 0u;
    uint32 index;

    if (mutable_pool == NULL || out_dirty == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    astra_latch_lock(&mutable_pool->latch);
    for (index = 0u; index < mutable_pool->frames.count; ++index) {
        if (mutable_pool->frames.frames[index].is_dirty) {
            ++count;
        }
    }
    astra_latch_unlock(&mutable_pool->latch);

    *out_dirty = count;
    return ASTRA_OK;
}

astra_status astra_buffer_pool_pin_count(const astra_buffer_pool *pool,
                                         page_id_t page_id,
                                         uint32 *out_pins)
{
    astra_buffer_pool *mutable_pool = (astra_buffer_pool *)(uintptr_t)pool;
    uint32 index;

    if (mutable_pool == NULL || out_pins == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    astra_latch_lock(&mutable_pool->latch);
    index = astra_page_table_find(&mutable_pool->table, mutable_pool->frames.frames, page_id);
    *out_pins = (index == ASTRA_FRAME_NONE) ? 0u
                                            : mutable_pool->frames.frames[index].pin_count;
    astra_latch_unlock(&mutable_pool->latch);

    return ASTRA_OK;
}

astra_status astra_buffer_pool_replacement_count(const astra_buffer_pool *pool,
                                                uint64 *out_clock_hits)
{
    astra_buffer_pool *mutable_pool = (astra_buffer_pool *)(uintptr_t)pool;

    if (mutable_pool == NULL || out_clock_hits == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    astra_latch_lock(&mutable_pool->latch);
    *out_clock_hits = mutable_pool->replacement_count;
    astra_latch_unlock(&mutable_pool->latch);

    return ASTRA_OK;
}

int astra_buffer_pool_describe(const astra_buffer_pool *pool, char *out, size_t out_size)
{
    astra_buffer_pool *mutable_pool = (astra_buffer_pool *)(uintptr_t)pool;
    uint64 used = 0u;
    uint64 dirty = 0u;
    uint64 retired;
    uint64 clock_hits;
    uint64 pins = 0u;
    uint32 index;

    if (out == NULL || out_size == 0u) {
        return 0;
    }
    if (mutable_pool == NULL) {
        return snprintf(out, out_size, "buffer pool: none");
    }

    /*
     * One latch hold for every number in the line, so the summary cannot describe a state
     * the pool was never actually in. A diagnostic that mixes two snapshots is worse than
     * one that is a moment stale, because it looks exact.
     *
     * `used` is read from the same counter astra_buffer_pool_used_frames reports, rather
     * than recounted from `in_table` here. A summary that counted the frames differently
     * to the query would print a number that disagreed with the API by one whenever a
     * reservation happened to be in flight, which is precisely the moment a caller is
     * most likely to be looking.
     */
    astra_latch_lock(&mutable_pool->latch);
    used = mutable_pool->frames.used;
    for (index = 0u; index < mutable_pool->frames.count; ++index) {
        if (mutable_pool->frames.frames[index].is_dirty) {
            ++dirty;
        }
        pins += mutable_pool->frames.frames[index].pin_count;
    }
    clock_hits = mutable_pool->replacement_count;
    retired = mutable_pool->retired.count;
    astra_latch_unlock(&mutable_pool->latch);

    return snprintf(out, out_size,
                    "buffer pool: %llu/%u frames used, %llu dirty, %llu pins, "
                    "%llu clock hits, %u byte pages, %llu retired",
                    (unsigned long long)used, mutable_pool->frames.count,
                    (unsigned long long)dirty, (unsigned long long)pins,
                    (unsigned long long)clock_hits, mutable_pool->page_size,
                    (unsigned long long)retired);
}
