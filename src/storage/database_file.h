/*
 * AstraDB storage: the primary data file.
 *
 * Private. This is the only translation unit in AstraDB that talks to the
 * operating system's file API, and it is the only one that touches raw on-disk
 * bytes. Everything above it works in pages and knows nothing about file
 * descriptors, offsets or byte order.
 *
 * What lives here
 * ---------------
 * - The POSIX and Win32 implementations of open, read, write, extend, truncate,
 *   flush and close. The two platforms are isolated behind this one interface so
 *   that no other module needs a preprocessor conditional.
 * - The header page: writing it, and parsing and validating it at open.
 * - The corruption checks that can be made without a higher layer to cooperate.
 *
 * What deliberately does not live here
 * ------------------------------------
 * Validation that needs to know what a *caller* asked for, such as "is this page
 * identifier inside the file" and "is this buffer the right size", is in page_io.c.
 * The split is by question rather than by convenience: this module answers "what
 * did the operating system do", the next one answers "was the request sensible".
 */
#ifndef ASTRA_STORAGE_DATABASE_FILE_H
#define ASTRA_STORAGE_DATABASE_FILE_H

#include "astra/core/error.h"
#include "astra/core/types.h"

#include <stdint.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * An open primary data file.
 *
 * `page_count` is a cache of the file length divided by the page size. The file is
 * only ever resized through this module and only in whole pages, and the handle is
 * single-owner, so re-stat-ing the file on every page access would buy nothing.
 * It is refreshed from the file itself on open, which is the only moment another
 * process could have changed it under us.
 */
typedef struct astra_database_file {
#if defined(_WIN32)
    /** Win32 file handle, INVALID_HANDLE_VALUE when closed. */
    HANDLE handle;
#else
    /** POSIX file descriptor, -1 when closed. */
    int fd;
#endif
    /** Page size in bytes. Fixed for the lifetime of the handle. */
    uint32 page_size;

    /** Number of pages in the file, always file_length / page_size. */
    uint64 page_count;

    /** Owned copy of the path, for diagnostics only. Never NULL while open. */
    char *path;
} astra_database_file;

/**
 * Creates a new file consisting of exactly one page: the header.
 *
 * Fails rather than overwriting if the path already exists.
 *
 * On success `*out_file` owns a file handle, a copy of `path` and nothing else.
 * `path` is not retained as a pointer: the copy means the caller may free or
 * modify its buffer as soon as the call returns.
 */
astra_status astra_file_create(const char *path,
                               uint32 page_size,
                               astra_database_file **out_file);

/**
 * Opens an existing file and validates it.
 *
 * A file is acceptable when it is at least one page long, its length is a whole
 * number of pages, page 0 carries the correct magic, its format version is
 * recognised, its recorded page size equals `expected_page_size`, and the header
 * checksum verifies. Anything else is reported as ASTRA_ERR_CORRUPTION, because
 * every one of those conditions means the file is not an AstraDB data file this
 * build can read.
 *
 * `expected_page_size` must already have been validated as a page size; the
 * comparison against the recorded size is exact rather than tolerant, because a
 * mismatch means one of the two sides is wrong and guessing which is not a
 * recovery, it is corruption with extra steps.
 */
astra_status astra_file_open(const char *path,
                             uint32 expected_page_size,
                             astra_database_file **out_file);

/**
 * Closes the file and releases everything `astra_file_open` or `astra_file_create`
 * allocated.
 *
 * NULL is allowed. Does not synchronise: the Disk Manager flushes before closing,
 * because it is the layer that knows whether a flush was wanted.
 */
void astra_file_close(astra_database_file *file);

/**
 * Reads exactly `size` bytes of page `page_id` into `dst`.
 *
 * `size` must equal the file's page size; a partial read is not an operation this
 * file layer offers, because nothing above it can use one. `page_id` must be
 * strictly less than `page_count`.
 */
astra_status astra_file_read_page(astra_database_file *file,
                                  page_id_t page_id,
                                  void *dst,
                                  uint32 size);

/**
 * Writes exactly `size` bytes of page `page_id` from `src`.
 *
 * Same contract as astra_file_read_page. The write reaches the operating system
 * but is not synchronised; see astra_file_sync.
 */
astra_status astra_file_write_page(astra_database_file *file,
                                   page_id_t page_id,
                                   const void *src,
                                   uint32 size);

/**
 * Sets the file length to `page_count` pages.
 *
 * Growing extends with zeros. Shrinking discards the pages beyond the target
 * permanently. Neither direction rewrites existing content, so the header page
 * survives a grow.
 */
astra_status astra_file_resize(astra_database_file *file, uint64 page_count);

/**
 * Asks the operating system to make the file's dirty blocks durable.
 *
 * This is fsync on POSIX and FlushFileBuffers on Windows. It does not synchronise
 * the directory entry that names the file, so a newly created database is not
 * guaranteed to survive a crash on POSIX; that limitation is stated in
 * astra_disk_manager_sync rather than hidden here.
 */
astra_status astra_file_sync(astra_database_file *file);

/** Queries the file length in bytes, without assuming it is a whole page count. */
astra_status astra_file_length(astra_database_file *file, uint64 *out_length);

/**
 * Returns true when `page_id` addresses a page that exists in `file`.
 *
 * The single place a page identifier is compared against a length, so that the
 * "not allocated" rule is written down once.
 */
bool astra_file_has_page(const astra_database_file *file, page_id_t page_id);

/**
 * Returns the byte offset of `page_id`, or false if the offset would overflow.
 *
 * The overflow check is not theoretical: `page_id` is a 64-bit value chosen by the
 * caller, and `page_id * page_size` can exceed the range of the platform's file
 * offsets long before the identifier itself does.
 */
bool astra_file_page_offset(page_id_t page_id,
                            uint32 page_size,
                            uint64 *out_offset);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_STORAGE_DATABASE_FILE_H */