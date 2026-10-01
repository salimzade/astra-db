/*
 * AstraDB core: process configuration.
 *
 * `astra_config` is a plain, caller owned value describing how the database
 * process should be set up. It holds no resources, no file handles and no
 * borrowed pointers, so copying it is a struct copy and destroying it is a no-op.
 *
 * Ownership
 * ---------
 * - The struct is always owned by the caller. AstraDB never retains a pointer
 *   to it. Passing a stack temporary, a heap block or a static object are all
 *   equally correct.
 * - `data_dir` is an inline array, so there is nothing to free and no separate
 *   string to keep alive.
 * - Every setter validates before mutating. On failure the struct is left
 *   exactly as it was.
 *
 * Scope of this phase
 * -------------------
 * Only the four settings that the current code can actually honour are present:
 * the data directory, the log level, the page size and the buffer pool size. There
 * is no parser and no configuration file format yet; adding one later means adding
 * a function that fills this struct, not changing the struct's meaning. The keys
 * those fields will eventually be read from are named here in their documented
 * spelling - `data_dir`, `log_level`, `page_size`, `buffer_pool_pages` - so that a
 * parser written later binds to names that have already been decided.
 */
#ifndef ASTRA_CORE_CONFIG_H
#define ASTRA_CORE_CONFIG_H

#include "astra/core/error.h"
#include "astra/core/log.h"
#include "astra/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of the data directory path, including the terminating NUL. */
#define ASTRA_CONFIG_PATH_MAX 1024

/** Data directory used when none is configured. Relative to the process working directory. */
#define ASTRA_CONFIG_DEFAULT_DATA_DIR "data"

/** Log level used when none is configured. */
#define ASTRA_CONFIG_DEFAULT_LOG_LEVEL ASTRA_LOG_INFO

/**
 * Smallest accepted buffer pool size, in pages.
 *
 * Two is the smallest pool that is worth configuring. A one frame "pool" is a Disk
 * Manager with an extra indirection: it caches exactly one page, and the first
 * fetch of a second page evicts the page the caller is most likely to want next.
 * Nothing about replacement can be observed in it, so accepting it would only
 * produce a configuration that cannot do what it was asked to do.
 */
#define ASTRA_BUFFER_POOL_PAGES_MIN ((uint32)2)

/**
 * Largest accepted buffer pool size, in pages.
 *
 * One million pages is 16 GiB at the default page size and 64 GiB at the largest
 * one the library accepts. That is far beyond any single-process pool, so the
 * limit exists to catch a mistyped `buffer_pool_pages` rather than to describe a
 * real ceiling; the constructor separately refuses any size whose byte total would
 * not fit in memory.
 */
#define ASTRA_BUFFER_POOL_PAGES_MAX ((uint32)1048576)

/** Buffer pool size used when none is configured explicitly. */
#define ASTRA_CONFIG_DEFAULT_BUFFER_POOL_PAGES ((uint32)4096)

/** Configuration value: data directory, log level, page size and buffer pool size. */
typedef struct astra_config {
    /**
     * Directory where the database keeps its files.
     *
     * Always NUL terminated. A relative path is resolved against the process
     * working directory when the database opens it, not when it is set. AstraDB
     * does not create or validate the directory here; that happens in the
     * storage phase, once there is something to store.
     */
    char data_dir[ASTRA_CONFIG_PATH_MAX];

    /** Minimum severity that will be logged. Applied via astra_log_set_level. */
    astra_log_level log_level;

    /**
     * Size of a storage page in bytes.
     *
     * Must satisfy astra_page_size_is_valid. Accepted by the configuration layer
     * and rejected by the storage layer, which may narrow the range once a page
     * layout exists.
     */
    uint32 page_size;

    /**
     * Number of pages the buffer pool caches in memory.
     *
     * Must be within [ASTRA_BUFFER_POOL_PAGES_MIN, ASTRA_BUFFER_POOL_PAGES_MAX].
     * Read by astra_buffer_pool_create and by nothing else: the setting is a size,
     * not a policy, and a pool that were resized at runtime would invalidate every
     * page pointer a caller holds, so the size is fixed for the life of a pool.
     */
    uint32 buffer_pool_pages;
} astra_config;

/**
 * Fills `cfg` with the built-in defaults.
 *
 * Defaults: data_dir = ASTRA_CONFIG_DEFAULT_DATA_DIR ("data"),
 * log_level = ASTRA_CONFIG_DEFAULT_LOG_LEVEL (ASTRA_LOG_INFO),
 * page_size = ASTRA_PAGE_SIZE_DEFAULT (16384),
 * buffer_pool_pages = ASTRA_CONFIG_DEFAULT_BUFFER_POOL_PAGES (4096).
 *
 * Parameters:
 *   cfg - destination. Must not be NULL.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `cfg` is NULL.
 *
 * Ownership: `cfg` remains owned by the caller. Allocates nothing.
 */
astra_status astra_config_init(astra_config *cfg);

/**
 * Validates every field of `cfg`.
 *
 * Applies the same rules as the individual setters: the data directory must be
 * non-empty and short enough to fit, the log level must be a member of
 * astra_log_level, the page size must satisfy astra_page_size_is_valid, and the
 * buffer pool size must be within [ASTRA_BUFFER_POOL_PAGES_MIN,
 * ASTRA_BUFFER_POOL_PAGES_MAX].
 *
 * Parameters:
 *   cfg - configuration to check. Must not be NULL.
 *
 * Returns:
 *   ASTRA_OK if the configuration is usable.
 *   ASTRA_ERR_INVALID_ARGUMENT if `cfg` is NULL.
 *   ASTRA_ERR_INVALID_ARGUMENT if `data_dir` is empty.
 *   ASTRA_ERR_OUT_OF_MEMORY if `data_dir` is not NUL terminated, meaning the
 *   struct was corrupted or hand built rather than produced by this API.
 *   ASTRA_ERR_INVALID_STATE if `log_level` is outside astra_log_level.
 *   ASTRA_ERR_UNSUPPORTED if `page_size` is outside the accepted range.
 *   ASTRA_ERR_UNSUPPORTED if `buffer_pool_pages` is outside the accepted range.
 *
 * Ownership: allocates nothing.
 */
astra_status astra_config_validate(const astra_config *cfg);

/**
 * Sets the data directory path.
 *
 * Parameters:
 *   cfg  - configuration to modify. Must not be NULL.
 *   path - NUL terminated path, copied into `cfg`. NULL is not allowed. Must be
 *          non-empty and at most ASTRA_CONFIG_PATH_MAX bytes including the NUL.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `cfg` or `path` is NULL, or if `path` is
 *   empty.
 *   ASTRA_ERR_OUT_OF_MEMORY if `path` does not fit in ASTRA_CONFIG_PATH_MAX.
 *
 * `cfg` is unchanged on failure.
 * Ownership: `path` is copied; the caller retains ownership of its buffer and
 * may free it immediately on success. Allocates nothing.
 */
astra_status astra_config_set_data_dir(astra_config *cfg, const char *path);

/**
 * Sets the log level.
 *
 * Parameters:
 *   cfg   - configuration to modify. Must not be NULL.
 *   level - minimum severity to log. Must be a member of astra_log_level.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `cfg` is NULL.
 *   ASTRA_ERR_INVALID_STATE if `level` is outside astra_log_level.
 *
 * `cfg` is unchanged on failure.
 * Ownership: allocates nothing.
 */
astra_status astra_config_set_log_level(astra_config *cfg, astra_log_level level);

/**
 * Sets the page size.
 *
 * Parameters:
 *   cfg   - configuration to modify. Must not be NULL.
 *   bytes - page size in bytes. Must satisfy astra_page_size_is_valid, that is
 *           be a multiple of ASTRA_PAGE_SIZE_MIN no greater than
 *           ASTRA_PAGE_SIZE_MAX.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `cfg` is NULL.
 *   ASTRA_ERR_UNSUPPORTED if `bytes` is outside the accepted range.
 *
 * `cfg` is unchanged on failure.
 * Ownership: allocates nothing.
 */
astra_status astra_config_set_page_size(astra_config *cfg, uint32 bytes);

/**
 * Sets the buffer pool size, in pages.
 *
 * The setting is read once, by astra_buffer_pool_create, and a pool's size never
 * changes afterwards. Resizing a live pool would have to either move every frame
 * or keep the frames where they are and stop promising that a page identifier maps
 * to a stable frame, and both are worse than refusing to resize: callers hold raw
 * pointers into frames, and nothing can repair those if the frames move.
 *
 * Parameters:
 *   cfg   - configuration to modify. Must not be NULL.
 *   pages - pool size in pages. Must be within
 *           [ASTRA_BUFFER_POOL_PAGES_MIN, ASTRA_BUFFER_POOL_PAGES_MAX].
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `cfg` is NULL.
 *   ASTRA_ERR_UNSUPPORTED if `pages` is outside the accepted range.
 *
 * `cfg` is unchanged on failure.
 * Ownership: allocates nothing.
 */
astra_status astra_config_set_buffer_pool_pages(astra_config *cfg, uint32 pages);

/**
 * Copies the contents of `src` into `dst`.
 *
 * Parameters:
 *   dst - destination. Must not be NULL.
 *   src - source. Must not be NULL.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `dst` or `src` is NULL.
 *
 * Ownership: allocates nothing; `src` and `dst` remain fully independent copies
 * afterwards.
 */
astra_status astra_config_copy(astra_config *dst, const astra_config *src);

/**
 * Writes a one line, human readable summary of `cfg` into `out`.
 *
 * Intended for a start up banner, not for machine parsing.
 *
 * Parameters:
 *   cfg      - configuration to render. NULL is allowed and renders as an
 *              unconfigured summary.
 *   out      - destination buffer. May be NULL only when `out_size` is 0.
 *   out_size - capacity in bytes including the terminating NUL.
 *
 * Returns:
 *   The number of bytes the full rendering would occupy, excluding the
 *   terminating NUL, following snprintf semantics. A value greater than or equal
 *   to `out_size` means the output was truncated and is NUL terminated.
 *
 * Ownership: writes into caller owned storage and allocates nothing.
 */
int astra_config_describe(const astra_config *cfg, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_CORE_CONFIG_H */