#include "astra/storage/disk_manager.h"

#include "astra/core/allocator.h"
#include "astra/core/log.h"
#include "storage/database_file.h"
#include "storage/page_io.h"
#include "storage/storage_internal.h"

#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

/*
 * The Disk Manager.
 *
 * This is the whole public storage surface, and it is small on purpose: create,
 * open, close, allocate, read, write, sync, truncate, and three queries. Every
 * other subsystem will reach the disk through these and nothing else, so anything
 * added here is something every future subsystem is stuck with.
 *
 * The module is mostly plumbing, and the plumbing exists to make the ownership
 * answerable. A handle owns exactly one file; the file owns exactly one path copy;
 * nothing else is retained after a call returns. The config is read and forgotten.
 * `astra_disk_manager_close` releases the handle whatever the flush reports, so
 * there is no path on which a handle outlives the function that was told to destroy
 * it.
 */

/**
 * An open database.
 *
 * Private by construction: the struct is defined here rather than in the public
 * header, so that adding a field - a free list, a lock, a statistics counter - is
 * not an ABI change and not a documented promise.
 */
struct astra_disk_manager {
    /** The primary data file. Owned. Never NULL while the handle is live. */
    astra_database_file *file;

    /** Cached copy of `file->page_size`, so validation costs no call. */
    uint32 page_size;
};

/*
 * ---------------------------------------------------------------------------
 * Paths and directories
 * ---------------------------------------------------------------------------
 */

astra_status astra_path_join(char *out, size_t out_size, const char *dir, const char *name)
{
    size_t dir_length;
    size_t name_length;
    bool needs_separator;
    size_t total;

    if (out == NULL || out_size == 0u || dir == NULL || name == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    dir_length = strlen(dir);
    name_length = strlen(name);

    if (name_length == 0u) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (dir_length == 0u) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    needs_separator = (dir[dir_length - 1u] != (char)ASTRA_DISK_MANAGER_PATH_SEPARATOR);
    total = dir_length + (needs_separator ? 1u : 0u) + name_length;

    if (total + 1u > out_size) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    memcpy(out, dir, dir_length);
    if (needs_separator) {
        out[dir_length] = (char)ASTRA_DISK_MANAGER_PATH_SEPARATOR;
    }
    memcpy(out + dir_length + (needs_separator ? 1u : 0u), name, name_length);
    out[total] = '\0';

    return ASTRA_OK;
}

astra_status astra_directory_create(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

#if defined(_WIN32)
    if (_mkdir(path) == 0) {
        return ASTRA_OK;
    }
    if (errno == EEXIST) {
        return ASTRA_OK;
    }
    if (errno == ENOENT) {
        return ASTRA_ERR_NOT_FOUND;
    }
    return ASTRA_ERR_IO;
#else
    if (mkdir(path, S_IRWXU) == 0) {
        return ASTRA_OK;
    }
    if (errno == EEXIST) {
        /*
         * A file, not a directory, also reports EEXIST here. Reporting success
         * would let the caller find out at file open time with a far less obvious
         * error, so the type is checked now.
         */
        struct stat info;
        if (stat(path, &info) == 0 && !S_ISDIR(info.st_mode)) {
            return ASTRA_ERR_IO;
        }
        return ASTRA_OK;
    }
    if (errno == ENOENT || errno == ENOTDIR) {
        return ASTRA_ERR_NOT_FOUND;
    }
    return ASTRA_ERR_IO;
#endif
}

/*
 * ---------------------------------------------------------------------------
 * Construction
 * ---------------------------------------------------------------------------
 */

/*
 * Validates the configuration and builds the path of the primary data file.
 *
 * `out_path` is a caller supplied buffer of ASTRA_STORAGE_PATH_MAX bytes, so no
 * allocation happens on the way in and the path never has to be freed.
 */
static astra_status resolve_database_path(const astra_config *cfg, char *out_path)
{
    astra_status status;

    if (cfg == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    status = astra_config_validate(cfg);
    if (status != ASTRA_OK) {
        return status;
    }

    return astra_path_join(out_path, ASTRA_STORAGE_PATH_MAX,
                           cfg->data_dir, ASTRA_DISK_MANAGER_PRIMARY_FILE);
}

/*
 * Wraps an opened file in a handle, or reports the failure that prevented it.
 *
 * The single place a handle comes into existence, so the "file open implies handle
 * allocated, and any failure releases both" rule is written once. `operation` is
 * "create" or "open" and appears only in log lines.
 */
static astra_status manager_adopt(astra_database_file *file,
                                  const char *operation,
                                  astra_disk_manager **out_manager)
{
    astra_disk_manager *manager = astra_alloc(sizeof *manager);

    if (manager == NULL) {
        astra_file_close(file);
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    manager->file = file;
    manager->page_size = file->page_size;

    *out_manager = manager;

    ASTRA_LOG_DEBUG(ASTRA_SUBSYSTEM_DISK, "%s %s: %llu page(s) of %lu bytes",
                    operation, file->path,
                    (unsigned long long)file->page_count,
                    (unsigned long)file->page_size);
    return ASTRA_OK;
}

astra_status astra_disk_manager_create(const astra_config *cfg,
                                       astra_disk_manager **out_manager)
{
    char path[ASTRA_STORAGE_PATH_MAX];
    astra_database_file *file = NULL;
    astra_status status;

    if (out_manager == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    *out_manager = NULL;

    status = resolve_database_path(cfg, path);
    if (status != ASTRA_OK) {
        return status;
    }

    /*
     * The directory is created before the file, and creating it is the only write
     * an open path never performs. The parent must already exist: see the header
     * for why the create is one level deep.
     */
    status = astra_directory_create(cfg->data_dir);
    if (status != ASTRA_OK) {
        return status;
    }

    status = astra_file_create(path, cfg->page_size, &file);
    if (status != ASTRA_OK) {
        return status;
    }

    ASTRA_LOG_INFO(ASTRA_SUBSYSTEM_DISK, "created %s: 1 page of %lu bytes",
                    path, (unsigned long)cfg->page_size);

    return manager_adopt(file, "create", out_manager);
}

astra_status astra_disk_manager_open(const astra_config *cfg,
                                     astra_disk_manager **out_manager)
{
    char path[ASTRA_STORAGE_PATH_MAX];
    astra_database_file *file = NULL;
    astra_status status;

    if (out_manager == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    *out_manager = NULL;

    status = resolve_database_path(cfg, path);
    if (status != ASTRA_OK) {
        return status;
    }

    /*
     * No directory is created here. Opening a database must not be able to bring
     * one into existence, or a typo in a path would silently produce an empty
     * database that reports success and then contains nothing.
     */
    status = astra_file_open(path, cfg->page_size, &file);
    if (status != ASTRA_OK) {
        if (status == ASTRA_ERR_CORRUPTION) {
            ASTRA_LOG_ERROR(ASTRA_SUBSYSTEM_DISK,
                            "%s is not a readable AstraDB data file for a %lu byte "
                            "page size", path, (unsigned long)cfg->page_size);
        }
        return status;
    }

    ASTRA_LOG_INFO(ASTRA_SUBSYSTEM_DISK, "opened %s: %llu page(s) of %lu bytes",
                    path, (unsigned long long)file->page_count,
                    (unsigned long)file->page_size);

    return manager_adopt(file, "open", out_manager);
}

astra_status astra_disk_manager_close(astra_disk_manager *manager)
{
    astra_status status;

    if (manager == NULL) {
        return ASTRA_OK;
    }

    /*
     * Flush first, close second, and report the flush afterwards. The order means a
     * caller cannot lose data by forgetting to sync; the reporting order means a
     * failed flush is still visible, even though the handle is gone by then and
     * there is nothing left for the caller to retry.
     */
    status = astra_file_sync(manager->file);
    if (status != ASTRA_OK) {
        ASTRA_LOG_ERROR(ASTRA_SUBSYSTEM_DISK,
                        "final synchronisation of %s failed; the database should be "
                        "treated as suspect", manager->file->path);
    }

    ASTRA_LOG_DEBUG(ASTRA_SUBSYSTEM_DISK, "closed %s", manager->file->path);

    astra_file_close(manager->file);
    astra_dealloc(manager);

    return status;
}

/*
 * ---------------------------------------------------------------------------
 * Queries
 * ---------------------------------------------------------------------------
 */

uint32 astra_disk_manager_page_size(const astra_disk_manager *manager)
{
    if (manager == NULL) {
        return ASTRA_PAGE_SIZE_DEFAULT;
    }
    return manager->page_size;
}

astra_status astra_disk_manager_file_size(const astra_disk_manager *manager,
                                          uint64 *out_bytes)
{
    if (manager == NULL || out_bytes == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    return astra_file_length(manager->file, out_bytes);
}

astra_status astra_disk_manager_page_count(const astra_disk_manager *manager,
                                           uint64 *out_pages)
{
    if (manager == NULL || out_pages == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    *out_pages = manager->file->page_count;
    return ASTRA_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Pages
 * ---------------------------------------------------------------------------
 */

astra_status astra_disk_manager_alloc_page(astra_disk_manager *manager,
                                           page_id_t *out_page_id)
{
    astra_status status;
    uint64 previous;

    if (manager == NULL || out_page_id == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    previous = manager->file->page_count;
    if (previous == UINT64_MAX) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    status = astra_file_resize(manager->file, previous + 1u);
    if (status != ASTRA_OK) {
        return status;
    }

    /*
     * The new page is the one that did not exist before, and the extension gave it
     * zero bytes. Writing zeros to it here would be a wasted page-sized write on
     * every allocation, and would change nothing a reader could observe.
     */
    *out_page_id = previous;

    return ASTRA_OK;
}

astra_status astra_disk_manager_read_page(astra_disk_manager *manager,
                                          page_id_t page_id,
                                          astra_page *page)
{
    if (manager == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    return astra_page_io_read(manager->file, page_id, page);
}

astra_status astra_disk_manager_write_page(astra_disk_manager *manager,
                                           astra_page *page)
{
    if (manager == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    return astra_page_io_write(manager->file, page);
}

/*
 * ---------------------------------------------------------------------------
 * Durability and file shape
 * ---------------------------------------------------------------------------
 */

astra_status astra_disk_manager_sync(astra_disk_manager *manager)
{
    if (manager == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    return astra_file_sync(manager->file);
}

astra_status astra_disk_manager_truncate(astra_disk_manager *manager,
                                          uint64 page_count)
{
    if (manager == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (page_count == 0u) {
        /* The header page is the database; a file without it cannot be opened. */
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    return astra_file_resize(manager->file, page_count);
}