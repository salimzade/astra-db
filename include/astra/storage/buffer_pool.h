/*
 * AstraDB storage: the Buffer Pool.
 *
 * The Buffer Pool is the layer between the Disk Manager and everything that will
 * read a page: a fixed number of page-sized frames held in memory, a page table
 * that maps a `page_id_t` onto the frame holding it, and a clock that decides which
 * frame to give up when a page is wanted and there is nowhere to put it.
 *
 * Why the page lifetime is spelled out this loudly
 * -------------------------------------------------
 * A buffer pool hands out a raw pointer into a buffer it owns, and that pointer
 * stops being valid at a moment the caller cannot observe. Most buffer pool bugs
 * are that bug: a page is fetched, the pointer is kept, the page is unpinned, and
 * the very next fetch evicts the frame. The buffer still exists, so nothing
 * crashes and the data is simply somebody else's.
 *
 * This API therefore makes the lifetime of a page the most constrained thing about
 * it. There is no way to obtain a frame pointer without taking a pin, there is no
 * way to release a pin other than through astra_buffer_pool_unpin_page, and
 * unpinning a page that holds no pins is an error rather than a silent decrement.
 * That last rule is the interlock: the classic way to corrupt a buffer pool is to
 * unpin twice, and here the second unpin reports ASTRA_ERR_INVALID_STATE instead of
 * leaving a page in use with a pin count of zero for the next fetch to evict.
 *
 * The rules, in full
 * ------------------
 * 1. `astra_buffer_pool_fetch_page` returns a page that is pinned. The pointer is
 *    valid only while that pin is held.
 * 2. `astra_buffer_pool_unpin_page` is the only thing that releases a pin, and
 *    every successful fetch needs exactly one matching unpin. The unpin takes the
 *    page identifier, not the pointer, so a page cannot be unpinned through a
 *    pointer that has already been recycled.
 * 3. A page with a pin count of zero may be evicted at any moment, including by a
 *    fetch on another thread. The frame and its buffer are reused, not freed, so
 *    the pointer does not become dangling - it silently starts describing a
 *    different page. That is precisely why the pointer must not be used after the
 *    unpin that released the last pin.
 * 4. `dirty` is declared at the unpin, not at the fetch. The pool cannot tell
 *    whether a page was modified from watching a buffer, so the caller states it.
 *    Passing `true` marks the page dirty; passing `false` leaves the existing dirty
 *    state alone rather than clearing it, because a second holder of the same page
 *    may have dirtied it.
 * 5. A page identifier names one page, so two concurrent fetches of the same
 *    identifier return the *same* frame and two pins. The buffer itself is not
 *    synchronised: a pinned page's bytes belong to the threads holding its pins, and
 *    a caller that needs two threads in one page at once has to exclude them
 *    itself. See Concurrency below.
 *
 * What this does not promise
 * --------------------------
 * No write-ahead log, so nothing here is atomic and nothing here is recoverable.
 * An eviction that flushes a dirty page is a plain file write: a crash part way
 * through leaves whatever the operating system managed to do, and no later open
 * will notice. That is the same limitation the Disk Manager states, and it is the
 * write-ahead log's job to remove.
 *
 * `astra_buffer_pool_delete_page` does not shrink the file. The Disk Manager has no
 * per-page free operation - a page is either allocated or the file is resized - so
 * deleting a page retires its identifier for the life of this pool handle and
 * discards its buffer. The bytes on disk are left alone. A later file compaction
 * is what will reclaim them; identifiers are never reused, so nothing can observe
 * the stale bytes through this pool.
 *
 * Concurrency
 * -----------
 * A pool is thread safe. Every entry point may be called concurrently from any
 * number of threads on the same pool, and a page fetched by one thread may be
 * unpinned by another.
 *
 * The pool serialises *metadata* - the page table, the pin counts, the clock hand -
 * behind one latch, and it never holds that latch across a disk read or a disk
 * write. A fetch that has to read a page from disk releases the latch first, so a
 * slow read does not block a concurrent flush. Writes of two different pages also
 * proceed in parallel, because a second, per-page latch serialises only two flushes
 * of the *same* page, which is the only case where they would race.
 *
 * What the pool does *not* synchronise is the contents of a page. The bytes in a
 * pinned frame belong to whoever holds the pin; the pool guarantees the frame will
 * not be recycled underneath them, and it makes no promise about two threads
 * writing the same buffer. A page-level latch for transactions does not exist yet
 * and is not faked here.
 *
 * Destroying a pool while another thread is inside it is undefined. That is not a
 * limitation of the implementation - it is the only sound rule, because a caller
 * holding a page pointer cannot be made to notice that its frame has been freed.
 * A caller that wants threads to stop must join them first.
 *
 * Ownership
 * ---------
 * - A pool is returned by `astra_buffer_pool_create` and owned by the caller, to be
 *   released with exactly one `astra_buffer_pool_destroy`.
 * - A pool *borrows* its `astra_disk_manager`. The Disk Manager must outlive every
 *   pool built on it, and destroying the pool does not close it. One manager may
 *   back several pools; the manager itself is still not thread safe, so that
 *   combination requires a pool which has serialised the access, which is to say one
 *   pool per manager.
 * - The pool owns its frames and every page buffer in them. A page pointer returned
 *   by a fetch is *borrowed*: it is owned by the pool and the caller must not
 *   release it, must not `astra_page_release` it, and must not keep it past the
 *   unpin that drops its last pin.
 * - `cfg` is read during `astra_buffer_pool_create` and is not retained, so a stack
 *   temporary is correct.
 * - The pool allocates one contiguous block for the frames and one for the page
 *   table, and the pinned set of deleted identifiers grows only as pages are
 *   deleted. All three are released by `astra_buffer_pool_destroy`.
 */
#ifndef ASTRA_STORAGE_BUFFER_POOL_H
#define ASTRA_STORAGE_BUFFER_POOL_H

#include "astra/core/config.h"
#include "astra/core/error.h"
#include "astra/core/types.h"
#include "astra/storage/disk_manager.h"
#include "astra/storage/page.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A pool of in-memory page frames. Opaque; the layout is private and may change. */
typedef struct astra_buffer_pool astra_buffer_pool;

/** Subsystem name reported by this module in errors and log records. */
#define ASTRA_SUBSYSTEM_BUFFER "buffer"

/*
 * ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/**
 * Creates a buffer pool over an open database.
 *
 * Allocates every frame up front, once. A pool that grew a frame on demand would
 * have to either block while it allocated - on a path that is supposed to be a
 * cache lookup - or fail the fetch, and a fetch that can fail for want of memory is
 * a fetch that has to be retried by every caller in the system. Fixing the size at
 * construction also makes a page identifier map to a stable frame for the life of
 * the pool, which is what lets a caller hold a frame pointer across unrelated calls.
 *
 * Parameters:
 *   cfg      - configuration to build from. Must not be NULL. `buffer_pool_pages`
 *              gives the number of frames and `page_size` must equal the page size
 *              of `disk`; a disagreement between the two is refused rather than
 *              resolved, because either one could be wrong and guessing is not a
 *              recovery. See Configuration below.
 *   disk     - open database to page through. Must not be NULL. Borrowed, and must
 *              outlive the pool. Must not already back another pool; see Ownership.
 *   out_pool - destination for the handle. Must not be NULL. Written only on
 *              success. On failure nothing is allocated.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if any argument is NULL, or if `disk` is not a live
 *           handle.
 *   ASTRA_ERR_UNSUPPORTED if the configuration is not valid, so that a pool is
 *           never built from a page size or a pool size the rest of the process
 *           would reject.
 *   ASTRA_ERR_OUT_OF_MEMORY if the frames, the page table or the pool itself could
 *           not be allocated, or if `buffer_pool_pages * page_size` would overflow a
 *           size_t.
 *   ASTRA_ERR_ALREADY_EXISTS if `disk` already backs a pool. See Ownership.
 *
 * Ownership: `*out_pool` is owned by the caller. `cfg` and `disk` are not retained
 * beyond what is stated above: `cfg` is not retained at all, and `disk` is borrowed.
 */
astra_status astra_buffer_pool_create(const astra_config *cfg,
                                      astra_disk_manager *disk,
                                      astra_buffer_pool **out_pool);

/**
 * Flushes every dirty page, then releases the pool.
 *
 * A page that is still pinned when the pool is destroyed is written out anyway, with
 * its pin count reported in a warning, because the alternative is silently
 * discarding a modification. It is still written: refusing to close would leave the
 * caller with a pool it cannot use and no way to flush the page itself.
 *
 * Callers must therefore treat a destroy as an assertion that they have finished
 * with the pool, not as a way to abandon it. Joins first, then destroy.
 *
 * Parameters:
 *   pool - pool to destroy. NULL is allowed and does nothing, so cleanup paths need
 *          no guard.
 *
 * Returns:
 *   ASTRA_OK on success, including for a NULL `pool`.
 *   ASTRA_ERR_IO if a page could not be written, or if the final synchronisation of
 *           the database failed. The pool is released regardless, because there is
 *           nothing a caller could retry with; the database should be treated as
 *           suspect.
 *
 * Ownership: consumes the pool. The caller must not use `pool` afterwards, must not
 * still hold any page pointer obtained from it, and must not destroy the disk manager
 * before this call returns.
 */
astra_status astra_buffer_pool_destroy(astra_buffer_pool *pool);

/*
 * ---------------------------------------------------------------------------
 * Pages
 * ---------------------------------------------------------------------------
 */

/**
 * Returns the page with identifier `page_id`, loading it if necessary, and pins it.
 *
 * On a hit the page is already in a frame: the pin count is incremented, the clock
 * hand is told the page was used, and the same frame is returned. On a miss the pool
 * either takes a frame that has never been used or evicts one - see astra_page *out
 * below - then reads the page from disk into it.
 *
 * A page that is not allocated is reported as ASTRA_ERR_NOT_FOUND, exactly as the
 * Disk Manager reports it. A page that was deleted through
 * astra_buffer_pool_delete_page is also reported as ASTRA_ERR_NOT_FOUND, even though
 * its bytes may still be in the file, so that a stale reference cannot resurrect it.
 *
 * The header page can be fetched: page 0 is a real page and tools will want to see
 * it. It arrives clean, and it is never written back, because the Disk Manager refuses
 * to write page 0 - its contents are the file's magic number and checksum. A caller
 * that modifies it and unpins with `dirty = true` has marked a page that can never be
 * flushed; astra_buffer_pool_flush_page and astra_buffer_pool_flush_all both report
 * that as ASTRA_ERR_INVALID_STATE rather than silently dropping the change. Read-only
 * use of the header page, which is what a tool wants, is unaffected.
 *
 * Parameters:
 *   pool     - pool to fetch from. Must not be NULL.
 *   page_id  - page to fetch. Must be a usable identifier; ASTRA_PAGE_ID_INVALID is
 *              reported as ASTRA_ERR_NOT_FOUND, not as a bad argument, because it
 *              names no page rather than describing a caller mistake.
 *   out_page - destination for the page. Must not be NULL. Written only on success.
 *              The page is pinned on return; see the rules at the top of this header.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `pool` or `out_page` is NULL.
 *   ASTRA_ERR_NOT_FOUND if `page_id` is not an allocated, undeleted page.
 *   ASTRA_ERR_INVALID_STATE if the pool is full and every frame is pinned. There is
 *           no frame to evict and the pool does not block, wait, or steal a pin: it
 *           reports that the caller is holding more pages than the pool can hold.
 *           Spreading the same work over fewer simultaneous pages, or raising
 *           `buffer_pool_pages`, is the fix.
 *   ASTRA_ERR_CORRUPTION if the file became shorter than the page being read.
 *   ASTRA_ERR_IO if the read failed.
 *   ASTRA_ERR_INTERNAL if the page table was exhausted, which cannot happen while
 *           every frame holds at most one page.
 *
 * Ownership: `*out_page` points into a frame owned by the pool. The caller does not
 * own it, must not release it, and must not use it after the unpin that drops its
 * last pin. Every successful call adds exactly one pin.
 */
astra_status astra_buffer_pool_fetch_page(astra_buffer_pool *pool,
                                          page_id_t page_id,
                                          astra_page **out_page);

/**
 * Appends a new page to the database, caches it, and returns it pinned.
 *
 * The page is zeroed, and is marked dirty before it is returned, because a page that
 * has been allocated but never written is indistinguishable on disk from one that
 * was written as zeros, and marking it dirty costs nothing while leaving it clean
 * would mean a caller that filled it in without unpinning with `dirty = true` would
 * silently lose its work at eviction.
 *
 * A new page needs a frame, so this fails with the same ASTRA_ERR_INVALID_STATE as a
 * fetch of a full pool when every frame is pinned. The Disk Manager's file is
 * extended before the frame is published, so a page that could not be allocated
 * leaves no frame behind and no identifier behind either: identifiers are only
 * assigned by a successful extend, and are never reused.
 *
 * Parameters:
 *   pool       - pool to allocate through. Must not be NULL.
 *   out_page   - destination for the new page. Must not be NULL. Written only on
 *                success. Pinned on return, like a fetched page.
 *   out_page_id - destination for the new identifier. Must not be NULL. Written only
 *                on success. The identifier is owned by the file, not by the caller:
 *                the caller uses it and never recycles it.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if any argument is NULL.
 *   ASTRA_ERR_INVALID_STATE if the pool is full and every frame is pinned.
 *   ASTRA_ERR_OUT_OF_MEMORY if the file could not be extended, or the pinned set of
 *           deleted identifiers could not grow. See the deletion note in the header.
 *   ASTRA_ERR_IO if the file could not be extended.
 *
 * Ownership: as for astra_buffer_pool_fetch_page. `*out_page_id` owns no memory.
 */
astra_status astra_buffer_pool_new_page(astra_buffer_pool *pool,
                                        astra_page **out_page,
                                        page_id_t *out_page_id);

/**
 * Releases one pin on a page, and optionally marks it dirty.
 *
 * Takes a page identifier rather than a pointer on purpose. A pointer cannot be
 * validated - by the time it is passed in, the frame it names may hold a different
 * page - so an API that accepted one would be trusting a value whose meaning has
 * already expired. The identifier is stable for the life of the database, so the
 * pool can check that the page is resident and that it still has a pin to give up.
 *
 * `dirty` is a declaration, not a mode. Passing `true` marks the page dirty and
 * `false` leaves it as it is; see rule 4 at the top of this header for why clearing
 * is not safe.
 *
 * Parameters:
 *   pool    - pool to unpin through. Must not be NULL.
 *   page_id - page to unpin. Must be resident in the pool and hold at least one pin.
 *   dirty   - true when the contents differ from what is on disk.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `pool` is NULL.
 *   ASTRA_ERR_NOT_FOUND if `page_id` is not resident. Either it was never fetched, or
 *           it was evicted, or it was deleted. Unpinning a page that is not resident
 *           cannot be a no-op, because the pin it would release is exactly what was
 *           supposed to have prevented the eviction.
 *   ASTRA_ERR_INVALID_STATE if `page_id` is resident with a pin count of zero. This
 *           is the double-unpin, and it is reported rather than tolerated: a page in
 *           use with no pin is one the next fetch is allowed to evict, which is the
 *           failure this API exists to make impossible.
 *
 * Ownership: allocates nothing. The caller keeps its page pointer for the duration of
 * the call and must not use it afterwards.
 */
astra_status astra_buffer_pool_unpin_page(astra_buffer_pool *pool,
                                          page_id_t page_id,
                                          bool dirty);

/*
 * ---------------------------------------------------------------------------
 * Writing back
 * ---------------------------------------------------------------------------
 */

/**
 * Writes one page to disk, whether or not it is dirty, and clears its dirty flag.
 *
 * A clean page is still written, because "flush this page" is a request about this
 * page and not about its state: a caller that has just modified the bytes without
 * saying so should be able to make them durable anyway. The write is not
 * synchronised; astra_disk_manager_sync is what makes it durable, and a page-by-page
 * fsync would make every other page write pay for a flush it did not ask for.
 *
 * The page may be pinned. Flushing a page that is in use is a normal thing to do - it
 * is how a checkpoint-style caller hands a page off - and the write takes the
 * page's write latch, so two concurrent flushes of the same page cannot interleave
 * their writes.
 *
 * A page must not be *modified* while a flush of it is in flight. The pool has no way
 * to tell a modification from a read of the buffer, so a modification during a flush
 * can be written in part or not at all, and the dirty flag is cleared on success
 * regardless. Holding the pin is the caller's tool for this: a thread that owns the
 * only pin on a page cannot race another thread's flush of it.
 *
 * Parameters:
 *   pool    - pool to flush through. Must not be NULL.
 *   page_id - page to write. Must be resident in the pool.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `pool` is NULL.
 *   ASTRA_ERR_NOT_FOUND if `page_id` is not resident. Flushing a page that is not
 *           cached would mean reading it in only to write it straight back, which is
 *           not an operation; a caller that wants that should use the Disk Manager.
 *   ASTRA_ERR_INVALID_STATE if the page is the header page, which the Disk Manager
 *           refuses to write.
 *   ASTRA_ERR_CORRUPTION or ASTRA_ERR_IO if the write failed.
 *
 * Ownership: allocates nothing. No *caller* pin is taken or released. The pool does pin
 * the frame internally for the duration of the write, so that a concurrent fetch
 * cannot evict a page whose bytes are in flight, and gives that pin back before
 * returning; the page's caller-visible pin count is unchanged by this call.
 */
astra_status astra_buffer_pool_flush_page(astra_buffer_pool *pool, page_id_t page_id);

/**
 * Writes every cached page to disk, clean or dirty, and synchronises the database.
 *
 * The synchronisation is part of the contract, not an extra: "flush everything" is
 * what a caller does before it stops caring, and making it also make the bytes
 * durable is the only reading in which the call is worth making.
 *
 * Pages are visited in frame order and each is written under the pool latch released,
 * so the call does not stop the world for the duration of the I/O. A page that
 * becomes resident after the call has passed its frame is not covered, which only
 * matters if another thread is using the pool at the same time; a single-threaded
 * caller gets every page that was cached when the call began.
 *
 * The header page is the one page that is never written, clean or not, because the
 * Disk Manager refuses to write it. A clean page 0 is skipped, and a *dirty* page 0 is
 * reported as ASTRA_ERR_INVALID_STATE before anything is written: it can only be dirty
 * if a caller uninned it with `dirty = true`, and refusing the whole call is more
 * honest than writing every other page and reporting success over a database whose
 * header is unwritable.
 *
 * Parameters:
 *   pool - pool to flush. Must not be NULL.
 *
 * Returns:
 *   ASTRA_OK on success, including when the pool holds no pages.
 *   ASTRA_ERR_INVALID_ARGUMENT if `pool` is NULL.
 *   ASTRA_ERR_INVALID_STATE if a resident page is the header page and is dirty. This
 *           cannot happen through the public API, and is reported rather than skipped
 *           so that a pool whose invariants are broken is never reported as flushed.
 *   ASTRA_ERR_CORRUPTION or ASTRA_ERR_IO if a write or the final synchronisation
 *           failed. The call stops at the first failure; pages already written are on
 *           disk and pages not yet visited are not.
 *
 * Ownership: allocates nothing. No caller pin is taken or released; each frame is
 * pinned internally while its bytes are in flight and unpinned afterwards, so a page
 * pinned by the caller is written rather than waited for.
 */
astra_status astra_buffer_pool_flush_all(astra_buffer_pool *pool);

/**
 * Retires a page: it is dropped from the pool and never served again.
 *
 * The page is removed from the page table and its frame is released for reuse. Its
 * dirty state is discarded rather than written, because a page being deleted is a
 * page whose contents are being abandoned; a page that was clean needs no write
 * either way. The identifier is recorded as retired for the life of this pool handle,
 * so a later fetch of it is reported as ASTRA_ERR_NOT_FOUND even though the bytes are
 * still in the file, and even if the file is later grown back over that offset.
 *
 * That set is not durable. There is no write-ahead log, so nothing records the
 * deletion and a reopened database sees whatever the file holds. A delete that must
 * survive a crash is the log's job, and this call is the point at which it would have
 * to be written.
 *
 * Parameters:
 *   pool    - pool to delete through. Must not be NULL.
 *   page_id - page to delete. Must not be ASTRA_PAGE_ID_INVALID.
 *
 * Returns:
 *   ASTRA_OK on success, including when the page was not cached. Deleting a page the
 *   pool does not hold is legal, because the caller may be deleting one it has
 *   already evicted; the retirement is the whole effect and it does not depend on the
 *   page being resident.
 *   ASTRA_ERR_INVALID_ARGUMENT if `pool` is NULL, or if `page_id` is
 *           ASTRA_PAGE_ID_INVALID, which names no page.
 *   ASTRA_ERR_INVALID_STATE if the page is currently pinned. A page in use cannot be
 *           pulled out from under its holder, and the pool will not wait for a pin to
 *           be released on the holder's behalf.
 *   ASTRA_ERR_INVALID_STATE if `page_id` is the header page. Page 0 belongs to the
 *           file; deleting it would leave something that is not a database.
 *   ASTRA_ERR_OUT_OF_MEMORY if the set of retired identifiers could not grow.
 *
 * Ownership: allocates nothing unless it has to grow the retired set, which the pool
 * owns and `astra_buffer_pool_destroy` releases.
 */
astra_status astra_buffer_pool_delete_page(astra_buffer_pool *pool, page_id_t page_id);

/*
 * ---------------------------------------------------------------------------
 * Queries
 * ---------------------------------------------------------------------------
 *
 * Queries take the pool latch only to read a counter, and none of them can block on
 * disk I/O.
 */

/**
 * Returns the number of frames the pool was built with.
 *
 * Fixed for the life of the pool. NULL is allowed and yields 0, so a caller can
 * divide by nothing rather than branch; it is not an invitation to skip the check.
 */
uint32 astra_buffer_pool_capacity(const astra_buffer_pool *pool);

/** Returns the page size of the database the pool pages through. NULL yields 0. */
uint32 astra_buffer_pool_page_size(const astra_buffer_pool *pool);

/**
 * Copies the number of frames currently holding a page into `out_used`.
 *
 * "Used" counts frames that hold a page, whether that page is pinned or not. A pool
 * is full when this equals astra_buffer_pool_capacity.
 */
astra_status astra_buffer_pool_used_frames(const astra_buffer_pool *pool,
                                           uint64 *out_used);

/**
 * Copies the number of resident dirty pages into `out_dirty`.
 *
 * This is the number of pages an eviction would have to write, and is therefore the
 * number of writes a checkpoint has avoided so far.
 */
astra_status astra_buffer_pool_dirty_frames(const astra_buffer_pool *pool,
                                            uint64 *out_dirty);

/**
 * Copies the number of pins held on `page_id` into `out_pins`.
 *
 * Reports 0 for a page that is not resident, which is the same answer a page that is
 * resident and fully unpinned gives. The distinction matters only to the pool, and
 * exposing it would invite callers to depend on the difference.
 */
astra_status astra_buffer_pool_pin_count(const astra_buffer_pool *pool,
                                         page_id_t page_id,
                                         uint32 *out_pins);

/**
 * Copies the number of frames the clock has swept past into `out_clock_hits`.
 *
 * Intended for tests and for the diagnostics a long running server prints when it is
 * asked why its hit rate is low. It is the clock's total, not a rate, because the pool
 * keeps no time base and a rate computed from a caller's stopwatch is the caller's
 * number anyway.
 */
astra_status astra_buffer_pool_replacement_count(const astra_buffer_pool *pool,
                                                uint64 *out_clock_hits);

/**
 * Writes a one line summary of the pool into `out`, for diagnostics.
 *
 * Not for machine parsing. Follows snprintf semantics: the return value is the
 * length the full summary would have needed, excluding the NUL, and a value at or
 * above `out_size` means the output was truncated and is NUL terminated.
 */
int astra_buffer_pool_describe(const astra_buffer_pool *pool, char *out, size_t out_size);

/*
 * ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 *
 * The pool size comes from `astra_config::buffer_pool_pages`, set with
 * astra_config_set_buffer_pool_pages, whose documented bounds are
 * ASTRA_BUFFER_POOL_PAGES_MIN and ASTRA_BUFFER_POOL_PAGES_MAX. It is read once, at
 * construction, and cannot be changed afterwards.
 *
 * The page size comes from the Disk Manager rather than from the configuration, and
 * the two are compared rather than reconciled. The Disk Manager is the authority
 * because it is the thing that has to read the file at a particular stride: a
 * configuration that says 16 KiB about a file whose header says 4 KiB means one of
 * the two is wrong, and there is no correct answer that does not involve asking the
 * user which one to believe.
 */
#ifdef __cplusplus
}
#endif

#endif /* ASTRA_STORAGE_BUFFER_POOL_H */
