/*
 * AstraDB storage: the page buffer.
 *
 * A page is the unit of everything below this header and the unit of everything
 * above it: the Disk Manager reads and writes whole pages, and the future buffer
 * pool will cache and evict whole pages. There is deliberately no "read these 40
 * bytes out of page 7" operation. A database that cannot perform an unaligned
 * partial page write has no torn records, no read-modify-write on a page it does
 * not own, and no second set of code paths to test, and none of those costs are
 * recoverable later without changing the format.
 *
 * What this is not
 * ----------------
 * This is not a buffer pool. `astra_page` owns one buffer and knows nothing about
 * other pages: there is no page table, no replacement policy, no pin count, no
 * frame reuse and no hash lookup. The `is_dirty` flag is the one hook the pool
 * will need and it is here so that the flag does not have to be retrofitted onto
 * every caller at the point the pool appears; the Disk Manager sets it to false
 * after a successful write and otherwise leaves it alone.
 *
 * Ownership
 * ---------
 * - An `astra_page` is a value. It is always owned by the caller, may live on the
 *   stack, and is never retained by the library.
 * - `astra_page::data` is owned by the page. `astra_page_init` allocates it and
 *   `astra_page_release` releases it; nothing else may free or realloc it.
 * - The Disk Manager borrows `data` for the duration of a read or write call and
 *   keeps no reference to it afterwards. A page handed to `astra_disk_manager_read_page`
 *   or `astra_disk_manager_write_page` may be released as soon as the call returns.
 * - `astra_page_release(NULL)` does nothing, and releasing an already released page
 *   is a no-op, so cleanup paths need no guard.
 */
#ifndef ASTRA_STORAGE_PAGE_H
#define ASTRA_STORAGE_PAGE_H

#include "astra/core/error.h"
#include "astra/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One page of the database, in memory.
 *
 * The struct is deliberately transparent: four fields, all of them state a caller
 * legitimately needs to inspect. Hiding them behind accessors would add a layer
 * of code that exists only to satisfy an abstraction nothing requires.
 */
typedef struct astra_page {
    /**
     * Identifier of this page.
     *
     * ASTRA_PAGE_ID_INVALID until `astra_page_init` gives it a real value, and
     * until then no write through the Disk Manager will be accepted. A read or a
     * write sets it to the page that was actually transferred.
     */
    page_id_t page_id;

    /**
     * Capacity of `data` in bytes.
     *
     * Always a page size accepted by `astra_page_size_is_valid`. The Disk Manager
     * requires this to equal *its* page size, so a page allocated for a 4 KiB
     * database cannot be written into a 16 KiB one by mistake.
     */
    uint32 data_size;

    /**
     * The raw page bytes, `data_size` of them.
     *
     * Owned by the page. NULL when the page is uninitialised or released.
     */
    void *data;

    /**
     * Whether the contents differ from what is on disk.
     *
     * The caller owns this flag: mark it when you modify `data`. The Disk Manager
     * never sets it to true, and clears it after a successful write. No subsystem
     * consults it yet.
     */
    bool is_dirty;
} astra_page;

/** A page in the uninitialised state: no identifier, no buffer. */
#define ASTRA_PAGE_INIT { ASTRA_PAGE_ID_INVALID, 0u, NULL, false }

/**
 * Initialises `page` with a zeroed buffer of `data_size` bytes.
 *
 * The buffer is zeroed rather than left uninitialised so that a page allocated
 * by the Disk Manager and written without being filled in reads back as zeros
 * instead of as whatever was on the heap. That makes the Disk Manager's behaviour
 * deterministic and is what the page level tests assert.
 *
 * Parameters:
 *   page      - destination. Must not be NULL. Overwritten, so any buffer the
 *               struct previously owned is leaked unless it was released first;
 *               call astra_page_release before re-initialising a live page.
 *   data_size - capacity of the buffer in bytes. Must satisfy
 *               astra_page_size_is_valid.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `page` is NULL.
 *   ASTRA_ERR_UNSUPPORTED if `data_size` is not an accepted page size.
 *   ASTRA_ERR_OUT_OF_MEMORY if the buffer could not be allocated. `page` is left
 *   in the uninitialised state.
 *
 * Ownership: allocates `data_size` bytes through `astra_alloc`, owned by `page`.
 * `page` must eventually be passed to astra_page_release.
 */
astra_status astra_page_init(astra_page *page, uint32 data_size);

/**
 * Releases the buffer owned by `page` and resets it to ASTRA_PAGE_INIT.
 *
 * Parameters:
 *   page - page to release. NULL is allowed and does nothing. Releasing an
 *          already released page is a no-op, so this is safe to call twice.
 *
 * Ownership: consumes the buffer. The caller must not use `page` afterwards
 * except to initialise or release it again.
 */
void astra_page_release(astra_page *page);

/**
 * Returns true when `page` owns a buffer and is therefore a usable argument.
 *
 * Intended as the guard before dereferencing `page->data`. Note that this says
 * nothing about whether the buffer is large enough for a particular file; only
 * `astra_disk_manager_read_page` and `astra_disk_manager_write_page` know that,
 * and they check `data_size` themselves.
 *
 * Parameters:
 *   page - page to test. NULL is allowed and yields false.
 *
 * Ownership: allocates nothing.
 */
bool astra_page_is_initialized(const astra_page *page);

/**
 * Fills `page->data` with `length` zero bytes.
 *
 * A named operation rather than a `memset` at every call site, because "this page
 * is now logically empty" is a storage decision that later changes when page types
 * arrive: at that point an empty heap page will still have to carry its header.
 *
 * Parameters:
 *   page   - page to clear. Must not be NULL and must own a buffer.
 *   length - number of bytes to zero. Must not exceed `page->data_size`.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `page` is NULL, `page->data` is NULL, or
 *   `length` exceeds `page->data_size`.
 *
 * Ownership: allocates nothing.
 */
astra_status astra_page_clear(astra_page *page, uint32 length);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_STORAGE_PAGE_H */