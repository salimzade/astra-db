/*
 * Feature test macros, before any system header.
 *
 * AstraDB compiles with C_EXTENSIONS OFF, which on glibc means -std=c17 and
 * therefore __STRICT_ANSI__. Under strict ANSI the POSIX declarations this file
 * depends on - pread, pwrite, fsync, ftruncate, open - are hidden, and their
 * absence shows up as an implicit declaration rather than as a clear error. The
 * macro therefore has to be the first thing in the translation unit, which is why
 * it sits above the include of the project's own header.
 */
#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _FILE_OFFSET_BITS
/* 64-bit file offsets on 32-bit hosts, so a large database is addressable there. */
#    define _FILE_OFFSET_BITS 64
#  endif
#endif

#include "storage/database_file.h"

#include "astra/core/allocator.h"
#include "astra/core/log.h"
#include "astra/storage/format.h"
#include "storage/storage_internal.h"

#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <stdio.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

/*
 * The primary data file.
 *
 * The two platforms differ in almost every line of the primitive section below and
 * in nothing else, so the platform difference is confined to six functions: open,
 * read at offset, write at offset, resize, flush, remove. Everything above them -
 * the header page, the corruption checks, the page count - is written once and is
 * portable.
 *
 * Nothing here allocates except the copy of the path and the single header buffer
 * used during create. That is deliberate: it means every failure path is a single
 * `file_release` call, and a caller can never be handed a half-open handle.
 */

/*
 * ---------------------------------------------------------------------------
 * Byte order
 * ---------------------------------------------------------------------------
 *
 * See storage_internal.h for why these are written a byte at a time.
 */

void astra_store_u32_le(uint8 *dst, uint32 value)
{
    dst[0] = (uint8)(value & 0xffu);
    dst[1] = (uint8)((value >> 8) & 0xffu);
    dst[2] = (uint8)((value >> 16) & 0xffu);
    dst[3] = (uint8)((value >> 24) & 0xffu);
}

void astra_store_u64_le(uint8 *dst, uint64 value)
{
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
        dst[shift / 8u] = (uint8)((value >> shift) & 0xffu);
    }
}

uint32 astra_load_u32_le(const uint8 *src)
{
    return (uint32)src[0]
         | ((uint32)src[1] << 8)
         | ((uint32)src[2] << 16)
         | ((uint32)src[3] << 24);
}

uint64 astra_load_u64_le(const uint8 *src)
{
    uint64 value = 0;

    for (unsigned index = 0; index < 8u; ++index) {
        value |= (uint64)src[index] << (index * 8u);
    }
    return value;
}

/*
 * The CRC-32 specified in format.h, computed bitwise.
 *
 * The table driven form is roughly eight times faster, and this file is the one
 * place in AstraDB that would benefit from one. It is still not worth it: a 1 KiB
 * lookup table would be the only sizeable piece of mutable global state in the
 * library, its construction would need its own initialisation and thread safety
 * story, and the checksum runs once per open rather than once per page. Bitwise
 * keeps the function pure, which is also why the tests can assert it against
 * published vectors with no setup at all.
 */
uint32 astra_checksum32(const uint8 *data, size_t length)
{
    uint32 crc = 0xffffffffu;

    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint32)data[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            /* Branch free: mask is all ones exactly when the low bit was set. */
            const uint32 mask = (uint32)0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }

    return ~crc;
}

/*
 * ---------------------------------------------------------------------------
 * Platform primitives
 * ---------------------------------------------------------------------------
 *
 * Each returns an astra_status and nothing else. None of them allocate, and none of
 * them fill in an astra_error: the status classifies the failure, and the
 * platform's own error text is not part of any contract this library offers.
 *
 * platform_read and platform_write are required to move the whole request or fail.
 * On POSIX a short transfer is retried, because a read of a regular file may
 * legally stop at a signal boundary. On Win32 a short transfer from a synchronous
 * read of a disk file means the file is not what it claimed to be, and retrying
 * would paper over exactly the condition this module exists to report.
 */

#if defined(_WIN32)

static astra_status platform_open(HANDLE *out_handle, const char *path, bool create)
{
    const DWORD access = GENERIC_READ | GENERIC_WRITE;
    const DWORD disposition = create ? CREATE_NEW : OPEN_EXISTING;
    HANDLE handle;

    /*
     * FILE_FLAG_RANDOM_ACCESS is a caching hint rather than a layout change, but it
     * is the accurate description of what this file is for. FILE_SHARE_READ is
     * deliberately absent: a database file that another process opens for writing
     * behind our back is a corruption source that no check at this layer can
     * detect, so the platform is asked to prevent it.
     */
    handle = CreateFileA(path,
                         access,
                         FILE_SHARE_READ,
                         NULL,
                         disposition,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                         NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        switch (GetLastError()) {
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return ASTRA_ERR_ALREADY_EXISTS;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ASTRA_ERR_NOT_FOUND;
        default:
            return ASTRA_ERR_IO;
        }
    }

    *out_handle = handle;
    return ASTRA_OK;
}

static void platform_close(HANDLE handle)
{
    (void)CloseHandle(handle);
}

static astra_status platform_read(HANDLE handle, uint64 offset, void *dst, uint32 size)
{
    LARGE_INTEGER position;
    DWORD moved = 0;

    position.QuadPart = (LONGLONG)offset;

    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN)) {
        return ASTRA_ERR_IO;
    }

    /* A page is at most 64 KiB, comfortably inside a DWORD, so one call suffices. */
    if (!ReadFile(handle, dst, (DWORD)size, &moved, NULL)) {
        return ASTRA_ERR_IO;
    }
    if (moved != (DWORD)size) {
        return ASTRA_ERR_CORRUPTION;
    }
    return ASTRA_OK;
}

static astra_status platform_write(HANDLE handle,
                                   uint64 offset,
                                   const void *src,
                                   uint32 size)
{
    LARGE_INTEGER position;
    DWORD moved = 0;

    position.QuadPart = (LONGLONG)offset;

    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN)) {
        return ASTRA_ERR_IO;
    }

    if (!WriteFile(handle, src, (DWORD)size, &moved, NULL)) {
        return ASTRA_ERR_IO;
    }
    if (moved != (DWORD)size) {
        return ASTRA_ERR_IO;
    }
    return ASTRA_OK;
}

static astra_status platform_resize(HANDLE handle, uint64 length)
{
    LARGE_INTEGER position;

    position.QuadPart = (LONGLONG)length;

    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN)) {
        return ASTRA_ERR_IO;
    }
    if (!SetEndOfFile(handle)) {
        return ASTRA_ERR_IO;
    }
    return ASTRA_OK;
}

static astra_status platform_sync(HANDLE handle)
{
    if (!FlushFileBuffers(handle)) {
        return ASTRA_ERR_IO;
    }
    return ASTRA_OK;
}

static astra_status platform_length(HANDLE handle, uint64 *out_length)
{
    LARGE_INTEGER length;

    if (!GetFileSizeEx(handle, &length)) {
        return ASTRA_ERR_IO;
    }
    if (length.QuadPart < 0) {
        return ASTRA_ERR_IO;
    }

    *out_length = (uint64)length.QuadPart;
    return ASTRA_OK;
}

static void platform_remove(const char *path)
{
    (void)DeleteFileA(path);
}

#else /* POSIX */

static astra_status platform_open(int *out_fd, const char *path, bool create)
{
    const int flags = O_RDWR | (create ? (O_CREAT | O_EXCL) : 0);
    const int fd = open(path, flags, S_IRUSR | S_IWUSR);

    if (fd < 0) {
        if (errno == EEXIST) {
            return ASTRA_ERR_ALREADY_EXISTS;
        }
        if (errno == ENOENT) {
            return ASTRA_ERR_NOT_FOUND;
        }
        return ASTRA_ERR_IO;
    }

    *out_fd = fd;
    return ASTRA_OK;
}

static void platform_close(int fd)
{
    (void)close(fd);
}

static astra_status platform_read(int fd, uint64 offset, void *dst, uint32 size)
{
    uint8 *cursor = (uint8 *)dst;
    uint32 done = 0;

    while (done < size) {
        const ssize_t got = pread(fd, cursor + done, (size_t)(size - done),
                                   (off_t)(offset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ASTRA_ERR_IO;
        }
        if (got == 0) {
            /* The file ended inside a page the cached length said existed. */
            return ASTRA_ERR_CORRUPTION;
        }
        done += (uint32)got;
    }
    return ASTRA_OK;
}

static astra_status platform_write(int fd, uint64 offset, const void *src, uint32 size)
{
    const uint8 *cursor = (const uint8 *)src;
    uint32 done = 0;

    while (done < size) {
        const ssize_t put = pwrite(fd, cursor + done, (size_t)(size - done),
                                    (off_t)(offset + done));
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ASTRA_ERR_IO;
        }
        if (put == 0) {
            /* Zero bytes is not progress, and retrying would spin forever. */
            return ASTRA_ERR_IO;
        }
        done += (uint32)put;
    }
    return ASTRA_OK;
}

static astra_status platform_resize(int fd, uint64 length)
{
    if (ftruncate(fd, (off_t)length) != 0) {
        return ASTRA_ERR_IO;
    }
    return ASTRA_OK;
}

static astra_status platform_sync(int fd)
{
    for (;;) {
        if (fsync(fd) == 0) {
            return ASTRA_OK;
        }
        if (errno == EINTR) {
            continue;
        }
        /*
         * EINVAL is what a filesystem without an fsync implementation returns, and
         * some return it for particular file kinds. Folding that into a generic I/O
         * error would make the database unusable on a filesystem that stores it
         * perfectly well, so the reason is logged and the caller is still told the
         * request was not honoured. The log line is the whole remedy available at
         * this layer; deciding what to do about it belongs above.
         */
        if (errno == EINVAL) {
            ASTRA_LOG_WARN(ASTRA_SUBSYSTEM_FILE,
                           "fsync is not supported for this file, so durability is "
                           "not guaranteed for it");
        }
        return ASTRA_ERR_IO;
    }
}

static astra_status platform_length(int fd, uint64 *out_length)
{
    struct stat info;

    if (fstat(fd, &info) != 0) {
        return ASTRA_ERR_IO;
    }
    if (!S_ISREG(info.st_mode)) {
        return ASTRA_ERR_CORRUPTION;
    }
    if (info.st_size < 0) {
        return ASTRA_ERR_IO;
    }

    *out_length = (uint64)info.st_size;
    return ASTRA_OK;
}

static void platform_remove(const char *path)
{
    (void)unlink(path);
}

#endif /* _WIN32 */

/*
 * The handle is a HANDLE on Windows and a descriptor on POSIX. Every use goes
 * through one of these two accessors so that the platform types never leak into the
 * portable half of the file, where they would be a portability bug waiting for the
 * next platform. One returns the value for the operations that need it, the other
 * returns its address for the one that has to store a freshly opened handle - the
 * handle starts out invalid, so there is no value to pass and only a slot to fill.
 */
#if defined(_WIN32)
static HANDLE file_handle(astra_database_file *file)
{
    return file->handle;
}

static HANDLE *file_handle_slot(astra_database_file *file)
{
    return &file->handle;
}
#else
static int file_handle(astra_database_file *file)
{
    return file->fd;
}

static int *file_handle_slot(astra_database_file *file)
{
    return &file->fd;
}
#endif

/*
 * ---------------------------------------------------------------------------
 * Header page
 * ---------------------------------------------------------------------------
 */

/*
 * Writes the header page into `buffer`, which must hold at least `page_size` bytes.
 *
 * The buffer is zeroed first and only then filled, so every reserved byte is
 * guaranteed zero on disk. That matters more than it looks: the checksum covers
 * the first 36 bytes only, so without this a file could carry whatever the
 * allocator happened to leave in the rest of the page, and two databases created
 * from identical operations would not be byte identical. That determinism is what
 * lets the format be tested by comparing bytes instead of by hoping.
 */
static void build_header_page(uint8 *buffer, uint32 page_size)
{
    uint32 checksum;

    memset(buffer, 0, (size_t)page_size);

    memcpy(buffer + ASTRA_FORMAT_OFFSET_MAGIC, ASTRA_FORMAT_MAGIC_TEXT,
           ASTRA_FORMAT_MAGIC_TEXT_SIZE);

    astra_store_u32_le(buffer + ASTRA_FORMAT_OFFSET_VERSION, ASTRA_FORMAT_VERSION);
    astra_store_u32_le(buffer + ASTRA_FORMAT_OFFSET_PAGE_SIZE, page_size);
    astra_store_u32_le(buffer + ASTRA_FORMAT_OFFSET_HEADER_BYTES, page_size);
    astra_store_u32_le(buffer + ASTRA_FORMAT_OFFSET_FLAGS, 0u);
    astra_store_u64_le(buffer + ASTRA_FORMAT_OFFSET_RESERVED, 0u);

    checksum = astra_checksum32(buffer, (size_t)ASTRA_FORMAT_CHECKSUM_SPAN);
    astra_store_u32_le(buffer + ASTRA_FORMAT_OFFSET_CHECKSUM, checksum);
}

/*
 * Validates the header page in `buffer` against `expected_page_size`.
 *
 * Every failure here is ASTRA_ERR_CORRUPTION, which is the honest classification
 * for all of them: a file whose magic does not match was not produced by AstraDB,
 * and a file whose magic matches but whose other fields do not was produced by
 * AstraDB and has been damaged or was written by a different build. Both are the
 * caller's "I cannot read this", and neither is repaired or ignored.
 *
 * The order is deliberate. Magic, then checksum, then everything the checksum
 * protects. Checking a field before the checksum would report a wrong reason for a
 * file whose header was damaged, and a caller acting on that reason would go
 * looking for the wrong problem.
 */
static astra_status verify_header_page(const uint8 *buffer, uint32 expected_page_size)
{
    uint32 stored_checksum;
    uint32 computed_checksum;
    uint32 recorded_page_size;
    uint32 header_bytes;
    uint32 flags;
    uint32 version;
    uint64 reserved;

    if (memcmp(buffer + ASTRA_FORMAT_OFFSET_MAGIC, ASTRA_FORMAT_MAGIC_TEXT,
               ASTRA_FORMAT_MAGIC_TEXT_SIZE) != 0) {
        return ASTRA_ERR_CORRUPTION;
    }
    if (buffer[ASTRA_FORMAT_OFFSET_MAGIC + ASTRA_FORMAT_MAGIC_TEXT_SIZE] != 0u) {
        return ASTRA_ERR_CORRUPTION;
    }

    stored_checksum = astra_load_u32_le(buffer + ASTRA_FORMAT_OFFSET_CHECKSUM);
    computed_checksum = astra_checksum32(buffer, (size_t)ASTRA_FORMAT_CHECKSUM_SPAN);
    if (stored_checksum != computed_checksum) {
        return ASTRA_ERR_CORRUPTION;
    }

    version = astra_load_u32_le(buffer + ASTRA_FORMAT_OFFSET_VERSION);
    if (version != ASTRA_FORMAT_VERSION) {
        return ASTRA_ERR_CORRUPTION;
    }

    recorded_page_size = astra_load_u32_le(buffer + ASTRA_FORMAT_OFFSET_PAGE_SIZE);
    if (recorded_page_size != expected_page_size) {
        return ASTRA_ERR_CORRUPTION;
    }

    header_bytes = astra_load_u32_le(buffer + ASTRA_FORMAT_OFFSET_HEADER_BYTES);
    if (header_bytes != expected_page_size) {
        return ASTRA_ERR_CORRUPTION;
    }

    flags = astra_load_u32_le(buffer + ASTRA_FORMAT_OFFSET_FLAGS);
    reserved = astra_load_u64_le(buffer + ASTRA_FORMAT_OFFSET_RESERVED);
    if (flags != 0u || reserved != 0u) {
        /* A reserved field this build does not understand has been set. */
        return ASTRA_ERR_CORRUPTION;
    }

    return ASTRA_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Handle lifetime
 * ---------------------------------------------------------------------------
 */

static void file_init_closed(astra_database_file *file, uint32 page_size, char *path)
{
#if defined(_WIN32)
    file->handle = INVALID_HANDLE_VALUE;
#else
    file->fd = -1;
#endif
    file->page_size = page_size;
    file->page_count = 0;
    file->path = path;
}

/*
 * Closes the handle if it is open and releases everything the file owns.
 *
 * Every failure path in this file ends here, exactly once, which is why it also
 * marks the handle closed before freeing: a caller cannot reach this function twice
 * for one handle, so it does not have to know whether the handle was open.
 */
static void file_release(astra_database_file *file)
{
#if defined(_WIN32)
    if (file->handle != INVALID_HANDLE_VALUE) {
        platform_close(file->handle);
        file->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (file->fd >= 0) {
        platform_close(file->fd);
        file->fd = -1;
    }
#endif

    astra_dealloc(file->path);
    file->path = NULL;
    astra_dealloc(file);
}

/*
 * Refreshes `page_count` from the file's actual length.
 *
 * A length that is not a whole number of pages is corruption: it means a write was
 * interrupted partway through a page while the file was being extended. There is
 * nothing to repair, and treating the trailing fragment as a page would hand the
 * caller bytes that are not there.
 */
static astra_status file_refresh_page_count(astra_database_file *file)
{
    uint64 length = 0;
    astra_status status;

    status = platform_length(file_handle(file), &length);
    if (status != ASTRA_OK) {
        return status;
    }

    if (length < (uint64)file->page_size ||
        (length % (uint64)file->page_size) != 0u) {
        return ASTRA_ERR_CORRUPTION;
    }

    file->page_count = length / (uint64)file->page_size;
    return ASTRA_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Public interface
 * ---------------------------------------------------------------------------
 */

bool astra_file_has_page(const astra_database_file *file, page_id_t page_id)
{
    if (file == NULL) {
        return false;
    }
    return page_id < file->page_count;
}

bool astra_file_page_offset(page_id_t page_id, uint32 page_size, uint64 *out_offset)
{
    if (out_offset == NULL || page_size == 0u) {
        return false;
    }

    /*
     * The limit is a page identifier whose byte offset still fits in 64 bits. This
     * is not a theoretical guard: page_id comes from the caller, and page_id times
     * page_size leaves the range of a file offset long before the identifier itself
     * is exhausted.
     */
    if (page_id > UINT64_MAX / (uint64)page_size) {
        return false;
    }

    *out_offset = page_id * (uint64)page_size;
    return true;
}

astra_status astra_file_create(const char *path,
                               uint32 page_size,
                               astra_database_file **out_file)
{
    astra_database_file *file;
    astra_status status;
    uint8 *header;

    if (path == NULL || out_file == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (!astra_page_size_is_valid(page_size)) {
        return ASTRA_ERR_UNSUPPORTED;
    }

    *out_file = NULL;

    file = astra_alloc(sizeof *file);
    if (file == NULL) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    file->path = astra_strdup(path);
    if (file->path == NULL) {
        astra_dealloc(file);
        return ASTRA_ERR_OUT_OF_MEMORY;
    }
    file_init_closed(file, page_size, file->path);

    status = platform_open(file_handle_slot(file), path, true);
    if (status != ASTRA_OK) {
        file_release(file);
        return status;
    }

    /*
     * Heap rather than stack because the page size is a runtime value bounded by
     * ASTRA_PAGE_SIZE_MAX: a fixed 64 KiB frame would be paid by every create to
     * serve a case that is never taken, and a variable length array is forbidden by
     * the warning set.
     */
    header = astra_alloc((size_t)page_size);
    if (header == NULL) {
        file_release(file);
        platform_remove(path);
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    build_header_page(header, page_size);
    status = platform_write(file_handle(file), 0u, header, page_size);
    astra_dealloc(header);

    if (status == ASTRA_OK) {
        /*
         * The header is not merely written, it is put on stable storage. A create
         * that reported success and then lost its header would leave a directory
         * that can never be opened again, which is the one failure this operation
         * must never produce. This is the only place in the library where a flush is
         * not the caller's decision, because the file does not usefully exist until
         * its header is durable.
         */
        status = platform_sync(file_handle(file));
    }

    if (status != ASTRA_OK) {
        file_release(file);
        /*
         * Remove the half-built file so that the directory is not left holding
         * something that can never be opened, and so a retry is possible. Removal is
         * best effort: if the underlying I/O is failing badly enough to reach here,
         * it may fail too, and that is reported by the status, not hidden.
         */
        platform_remove(path);
        return status;
    }

    file->page_count = 1u;

    *out_file = file;
    return ASTRA_OK;
}

astra_status astra_file_open(const char *path,
                             uint32 expected_page_size,
                             astra_database_file **out_file)
{
    astra_database_file *file;
    astra_status status;
    uint8 *header;

    if (path == NULL || out_file == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (!astra_page_size_is_valid(expected_page_size)) {
        return ASTRA_ERR_UNSUPPORTED;
    }

    *out_file = NULL;

    file = astra_alloc(sizeof *file);
    if (file == NULL) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    file->path = astra_strdup(path);
    if (file->path == NULL) {
        astra_dealloc(file);
        return ASTRA_ERR_OUT_OF_MEMORY;
    }
    file_init_closed(file, expected_page_size, file->path);

    status = platform_open(file_handle_slot(file), path, false);
    if (status != ASTRA_OK) {
        file_release(file);
        return status;
    }

    /*
     * Length before header, always. If the file is shorter than a single page there
     * is no header to read, and reading page 0 would be reading past the end of the
     * file and getting back zeros that look like a plausible header.
     */
    status = file_refresh_page_count(file);
    if (status != ASTRA_OK) {
        file_release(file);
        return status;
    }

    header = astra_alloc((size_t)expected_page_size);
    if (header == NULL) {
        file_release(file);
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    status = platform_read(file_handle(file), 0u, header, expected_page_size);
    if (status == ASTRA_OK) {
        status = verify_header_page(header, expected_page_size);
    }
    astra_dealloc(header);

    if (status != ASTRA_OK) {
        file_release(file);
        return status;
    }

    *out_file = file;
    return ASTRA_OK;
}

void astra_file_close(astra_database_file *file)
{
    if (file == NULL) {
        return;
    }
    file_release(file);
}

astra_status astra_file_read_page(astra_database_file *file,
                                  page_id_t page_id,
                                  void *dst,
                                  uint32 size)
{
    uint64 offset = 0;

    if (file == NULL || dst == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (size != file->page_size) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (!astra_file_has_page(file, page_id)) {
        return ASTRA_ERR_NOT_FOUND;
    }
    if (!astra_file_page_offset(page_id, file->page_size, &offset)) {
        return ASTRA_ERR_NOT_FOUND;
    }

    return platform_read(file_handle(file), offset, dst, size);
}

astra_status astra_file_write_page(astra_database_file *file,
                                   page_id_t page_id,
                                   const void *src,
                                   uint32 size)
{
    uint64 offset = 0;

    if (file == NULL || src == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (size != file->page_size) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (!astra_file_has_page(file, page_id)) {
        return ASTRA_ERR_NOT_FOUND;
    }
    if (!astra_file_page_offset(page_id, file->page_size, &offset)) {
        return ASTRA_ERR_NOT_FOUND;
    }

    return platform_write(file_handle(file), offset, src, size);
}

astra_status astra_file_resize(astra_database_file *file, uint64 page_count)
{
    uint64 length = 0;
    astra_status status;

    if (file == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (page_count == 0u) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (!astra_file_page_offset(page_count, file->page_size, &length)) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    status = platform_resize(file_handle(file), length);
    if (status != ASTRA_OK) {
        return status;
    }

    /*
     * The cached count is taken from the request rather than re-read from the file.
     * Both are equivalent, because platform_resize is one operation and the file is
     * single-owner; re-reading would only add a way for the two to disagree.
     */
    file->page_count = page_count;
    return ASTRA_OK;
}

astra_status astra_file_sync(astra_database_file *file)
{
    if (file == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    return platform_sync(file_handle(file));
}

astra_status astra_file_length(astra_database_file *file, uint64 *out_length)
{
    uint64 length = 0;
    astra_status status;

    if (file == NULL || out_length == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    status = platform_length(file_handle(file), &length);
    if (status != ASTRA_OK) {
        return status;
    }

    *out_length = length;
    return ASTRA_OK;
}