/*
 * AstraDB storage: the Disk Manager.
 *
 * The Disk Manager is the only thing in AstraDB that turns a `page_id_t` into
 * bytes on a disk and back. Everything above it (the buffer pool, heap files,
 * B+Tree nodes, the catalog) will speak in pages; everything below it (the
 * operating system) does not know what a page is. This header is that boundary.
 *
 * The contract
 * ------------
 * - A database is a directory. The primary data file is `<data_dir>/main.db`.
 * - Pages are fixed size, and the page size comes from configuration. It is
 *   recorded in the file, and a file whose recorded page size disagrees with the
 *   configuration is refused.
 * - A page with identifier N is at byte offset N * page_size, so identifiers are
 *   stable and are never reused by an append.
 * - Reads and writes move whole pages. There is no partial page entry point, and
 *   no way to obtain the file handle and write to it behind this API's back.
 * - Every argument is validated: the handle, the page identifier against the file's
 *   real length, the buffer pointer, the buffer size against the file's page size.
 *   A rejected call changes nothing.
 *
 * What it does not promise
 * ------------------------
 * The Disk Manager persists pages and can be asked to synchronise them. That is
 * all. It provides no atomicity, no ordering between pages, no torn write
 * detection on data pages and no recovery. A crash between two writes can leave
 * one of them applied and the other not. Everything beyond "the bytes you asked
 * for are on the disk" is the write-ahead log's responsibility, and there is no
 * write-ahead log yet. Treat `astra_disk_manager_sync` as "please flush", not as
 * "this is now a consistent database".
 *
 * Concurrency
 * -----------
 * An `astra_disk_manager` is not thread safe. There is one file handle, one
 * cached page count and no lock; a handle must be used by one thread at a time,
 * and a handle must be closed before another thread may use its file. Two handles
 * onto the same database directory are independent objects but not independent
 * writers: nothing here serialises them, and a database is not meant to be opened
 * twice at once. This is documented rather than locked because the buffer pool,
 * which will own the locking policy, does not exist yet.
 *
 * Ownership
 * ---------
 * - `astra_disk_manager_create` and `astra_disk_manager_open` return a handle
 *   owned by the caller, to be released with exactly one
 *   `astra_disk_manager_close`.
 * - The handle owns the file, and the file owns a copy of its path. The library
 *   retains nothing the caller passed in: `cfg` is read during the call and is not
 *   kept, and its `data_dir` buffer may be modified or freed as soon as the call
 *   returns.
 * - Pages are caller owned and are borrowed only for the duration of a call. See
 *   `astra/storage/page.h`.
 */
#ifndef ASTRA_STORAGE_DISK_MANAGER_H
#define ASTRA_STORAGE_DISK_MANAGER_H

#include "astra/core/config.h"
#include "astra/core/error.h"
#include "astra/core/types.h"
#include "astra/storage/format.h"
#include "astra/storage/page.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * File name of the primary data file inside the database directory.
 *
 * "main" rather than "data" or "database" because this file is the primary one:
 * naming it that way leaves room for `wal`, `index` and friends to be added
 * beside it without renaming a file that already exists on someone's disk. With
 * the default data directory the path is `data/main.db`.
 */
#define ASTRA_DISK_MANAGER_PRIMARY_FILE "main.db"

/** Separator used to join the data directory to the primary file name. */
#define ASTRA_DISK_MANAGER_PATH_SEPARATOR '/'

/** An open database. Opaque; the layout is private and may change. */
typedef struct astra_disk_manager astra_disk_manager;

/*
 * ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/**
 * Creates a database directory and its primary data file, then opens it.
 *
 * The data directory is created if it does not exist. Only the final path
 * component is created: intermediate directories must already exist, because a
 * recursive create needs path parsing rules that differ between platforms and no
 * subsystem needs it yet.
 *
 * Parameters:
 *   cfg       - configuration to create from. Must not be NULL. `data_dir` names
 *               the directory to create; `page_size` fixes the page size. See
 *               Configuration below.
 *   out_manager - destination for the handle. Must not be NULL. Written only on
 *               success. On failure nothing is allocated and no file is created.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `cfg` or `out_manager` is NULL.
 *   ASTRA_ERR_INVALID_ARGUMENT if the configuration is not valid, so that a bad
 *   page size is reported before a file exists on disk.
 *   ASTRA_ERR_OUT_OF_MEMORY if `<data_dir>/main.db` does not fit in the path
 *           buffer, or if the handle could not be allocated.
 *   ASTRA_ERR_ALREADY_EXISTS if the primary data file already exists. An existing
 *   database is opened with astra_disk_manager_open, never overwritten.
 *   ASTRA_ERR_IO if the directory could not be created or the file not created.
 *
 * Ownership: `*out_manager` is owned by the caller. `cfg` is not retained.
 */
astra_status astra_disk_manager_create(const astra_config *cfg,
                                       astra_disk_manager **out_manager);

/**
 * Opens an existing database directory.
 *
 * Performs no writes of any kind. In particular it does not create the data
 * directory: opening a database that is not there reports ASTRA_ERR_NOT_FOUND
 * rather than silently laying down an empty one.
 *
 * The file is checked before the handle is handed back. See Corruption below.
 *
 * Parameters:
 *   cfg         - configuration to open with. Must not be NULL. `page_size` must
 *                 match the page size recorded in the file.
 *   out_manager - destination for the handle. Must not be NULL. Written only on
 *                 success.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `cfg` or `out_manager` is NULL.
 *   ASTRA_ERR_INVALID_ARGUMENT if the configuration is not valid.
 *   ASTRA_ERR_OUT_OF_MEMORY if the path does not fit in the path buffer, or the
 *           handle could not be allocated.
 *   ASTRA_ERR_NOT_FOUND if the directory or the primary data file does not exist.
 *   ASTRA_ERR_CORRUPTION if the file is not an AstraDB data file, is shorter than
 *           one page, its length is not a whole number of pages, its format
 *           version is not recognised, its recorded page size does not match
 *           `cfg->page_size`, or its header checksum does not verify.
 *   ASTRA_ERR_IO if the file could not be opened or queried.
 *
 * Ownership: `*out_manager` is owned by the caller. `cfg` is not retained.
 */
astra_status astra_disk_manager_open(const astra_config *cfg,
                                     astra_disk_manager **out_manager);

/**
 * Synchronises, closes and releases a database handle.
 *
 * Synchronisation is attempted first so that a caller cannot lose data by
 * forgetting to flush, but the handle is released either way: a failed flush must
 * not leak a file handle, and there is nothing a caller could do with the handle
 * afterwards that would recover the situation.
 *
 * Parameters:
 *   manager - handle to close. NULL is allowed and does nothing.
 *
 * Returns:
 *   ASTRA_OK on success, including for a NULL `manager`.
 *   ASTRA_ERR_IO if the final synchronisation failed. The handle is gone
 *           regardless; the data may or may not have reached stable storage and
 *           the database should be treated as suspect.
 *
 * Ownership: consumes the handle. The caller must not use `manager` afterwards.
 */
astra_status astra_disk_manager_close(astra_disk_manager *manager);

/*
 * ---------------------------------------------------------------------------
 * Queries
 * ---------------------------------------------------------------------------
 */

/**
 * Returns the page size of an open database.
 *
 * Parameters:
 *   manager - handle to query. NULL is allowed and yields
 *             ASTRA_PAGE_SIZE_DEFAULT, so that a caller can size a page buffer
 *             from a configuration without branching on whether a database is
 *             open. That is a convenience, not an invitation to skip the check:
 *             an open database always has a fixed size that must be used.
 *
 * Returns: the page size in bytes. Never fails and allocates nothing.
 */
uint32 astra_disk_manager_page_size(const astra_disk_manager *manager);

/**
 * Copies the length of the primary data file into `out_bytes`.
 *
 * Parameters:
 *   manager  - handle to query. Must not be NULL.
 *   out_bytes - destination. Must not be NULL. Written only on success. Always a
 *               whole multiple of the page size, because the Disk Manager only
 *               ever grows the file by whole pages.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `manager` or `out_bytes` is NULL.
 *
 * Ownership: allocates nothing.
 */
astra_status astra_disk_manager_file_size(const astra_disk_manager *manager,
                                          uint64 *out_bytes);

/**
 * Copies the number of pages in the primary data file into `out_pages`.
 *
 * Equal to `file_size / page_size`, which is where the number comes from. A newly
 * created database reports 1, because the header page exists.
 *
 * Parameters:
 *   manager   - handle to query. Must not be NULL.
 *   out_pages - destination. Must not be NULL. Written only on success.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `manager` or `out_pages` is NULL.
 *
 * Ownership: allocates nothing.
 */
astra_status astra_disk_manager_page_count(const astra_disk_manager *manager,
                                           uint64 *out_pages);

/*
 * ---------------------------------------------------------------------------
 * Pages
 * ---------------------------------------------------------------------------
 */

/**
 * Appends one page to the end of the file and returns its identifier.
 *
 * Allocation is an append, so identifiers are dense, increasing by one, and never
 * recycled: a page that is truncated away does not come back. That is the property
 * the buffer pool's page table and every later subsystem depend on, and it is why
 * allocation cannot be implemented as a search for a hole.
 *
 * The new page reads back as zeros. It is not written through to disk as a
 * separate step; extending the file is what makes the page exist, and a page of
 * zeros is indistinguishable on disk from a page that was written as zeros.
 *
 * Parameters:
 *   manager      - handle to allocate from. Must not be NULL.
 *   out_page_id  - destination for the new identifier. Must not be NULL. Written
 *                  only on success.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `manager` or `out_page_id` is NULL.
 *   ASTRA_ERR_OUT_OF_MEMORY if the file length would overflow the largest file
 *           the platform supports, or the handle could not be allocated.
 *   ASTRA_ERR_IO if the file could not be extended.
 *
 * Ownership: allocates nothing. The returned identifier owns no memory.
 */
astra_status astra_disk_manager_alloc_page(astra_disk_manager *manager,
                                           page_id_t *out_page_id);

/**
 * Reads one page from disk into `page`.
 *
 * On success `page->data_size` is unchanged, `page->data` holds exactly the page
 * size the database was opened with, `page->page_id` is the identifier that was
 * read, and `page->is_dirty` is false.
 *
 * Parameters:
 *   manager  - handle to read from. Must not be NULL.
 *   page_id  - page to read. Must be a usable identifier strictly less than the
 *              page count. Page 0 may be read: the header page is a real page and
 *              tools will want to see it.
 *   page     - destination. Must not be NULL and must own a buffer whose
 *              `data_size` equals this database's page size.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if any argument is NULL, if `page` owns no buffer,
 *           or if `page->data_size` does not match this database's page size. A
 *           page sized for a different database is a caller bug, not corruption.
 *   ASTRA_ERR_NOT_FOUND if `page_id` is ASTRA_PAGE_ID_INVALID or is beyond the end
 *           of the file. The page has not been allocated.
 *   ASTRA_ERR_CORRUPTION if the file changed underneath the handle and is now
 *           shorter than the requested page.
 *   ASTRA_ERR_IO if the read failed.
 *
 * Ownership: borrows `page->data` for the duration of the call and keeps no
 * reference to it. `page` is not modified on failure.
 */
astra_status astra_disk_manager_read_page(astra_disk_manager *manager,
                                          page_id_t page_id,
                                          astra_page *page);

/**
 * Writes one page to disk.
 *
 * The whole page is written; there is no partial write. On success
 * `page->is_dirty` is false, because the bytes on disk now match the buffer. That
 * is why `page` is not const: leaving a page marked dirty after its bytes are
 * safely on disk is how a later write gets skipped for no reason.
 *
 * The write is not synchronised: call astra_disk_manager_sync when durability is
 * needed, which a caller cannot do implicitly because a page-by-page fsync would
 * make every other page write pay for a flush it did not ask for.
 *
 * Parameters:
 *   manager - handle to write to. Must not be NULL.
 *   page    - page to write. Must not be NULL, must own a buffer whose
 *             `data_size` equals this database's page size, and must carry a page
 *             identifier that has already been allocated.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if any argument is NULL, if `page` owns no buffer,
 *           or if `page->data_size` does not match this database's page size.
 *   ASTRA_ERR_NOT_FOUND if `page->page_id` has not been allocated.
 *   ASTRA_ERR_INVALID_STATE if `page->page_id` is the header page. Page 0 belongs
 *           to the file, and a write through this API would invalidate the magic
 *           number and the checksum, leaving a database that can no longer be
 *           opened. Its contents are fixed for the life of the format.
 *   ASTRA_ERR_IO if the write failed.
 *
 * Ownership: borrows `page->data` for the duration of the call. `page->is_dirty` is
 * set to false on success and left alone on failure.
 */
astra_status astra_disk_manager_write_page(astra_disk_manager *manager,
                                           astra_page *page);

/*
 * ---------------------------------------------------------------------------
 * Durability and file shape
 * ---------------------------------------------------------------------------
 */

/**
 * Asks the operating system to make every page written so far durable.
 *
 * What this guarantees: after a successful return, the operating system has been
 * asked to write the file's dirty blocks to stable storage, so the pages written
 * before the call survive the loss of the process.
 *
 * What it does not guarantee, stated plainly because the difference matters:
 * nothing about the operating system or the hardware honouring the request, no
 * atomicity across pages, no ordering relative to writes on other files or
 * descriptors, no protection against a page write that was interrupted partway,
 * and no repair of a database that was already inconsistent. Durability beyond
 * this point is the WAL's job and the WAL does not exist yet.
 *
 * Parameters:
 *   manager - handle to flush. Must not be NULL.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `manager` is NULL.
 *   ASTRA_ERR_IO if the operating system call failed.
 *
 * Ownership: allocates nothing.
 */
astra_status astra_disk_manager_sync(astra_disk_manager *manager);

/**
 * Sets the file to exactly `page_count` pages, discarding any beyond it.
 *
 * Growing is safe and simply extends the file with zero pages, equivalent to
 * allocating them one at a time. Shrinking discards the pages at the end
 * permanently; their identifiers are never reused, because reusing them would
 * make a stale buffer pool entry silently address different data.
 *
 * This exists because a file's shape has to be settable for backup, restore and
 * file compaction, all of which are later phases. No higher layer calls it yet,
 * and it is not transactional: a crash in the middle of a shrink leaves whatever
 * the operating system managed to do. Callers that need the discarded pages to
 * survive a crash need the WAL, which does not exist yet.
 *
 * Parameters:
 *   manager    - handle to resize. Must not be NULL.
 *   page_count - target length in pages. Must be at least 1, so the header page
 *                cannot be removed, and small enough that `page_count * page_size`
 *                fits the platform's maximum file length.
 *
 * Returns:
 *   ASTRA_OK on success, including when the file is already that long.
 *   ASTRA_ERR_INVALID_ARGUMENT if `manager` is NULL.
 *   ASTRA_ERR_INVALID_ARGUMENT if `page_count` is 0.
 *   ASTRA_ERR_OUT_OF_MEMORY if the target length overflows.
 *   ASTRA_ERR_IO if the operating system call failed. The file may have been
 *           resized even though the call reports failure; re-query the size
 *           before drawing conclusions.
 *
 * Ownership: allocates nothing.
 */
astra_status astra_disk_manager_truncate(astra_disk_manager *manager,
                                          uint64 page_count);

/*
 * ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 */

/*
 * The Disk Manager takes an `astra_config` rather than a page size and a path
 * because those are exactly the settings the configuration exists to carry, and
 * because making it take a config means the storage layer cannot be constructed
 * with a page size that the rest of the process would disagree about. The
 * configuration is validated in full on the way in, so an unusable page size is
 * reported before any file is touched.
 *
 * The database directory must exist, or be creatable as a single component, when
 * creating. It must exist when opening.
 */

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_STORAGE_DISK_MANAGER_H */