#include "astra/core/config.h"

#include "core/astra_internal.h"

#include <stdio.h>
#include <string.h>

/*
 * Process configuration.
 *
 * The struct is a value, so this module is almost entirely validation. The rule
 * that shapes it: every setter validates before it mutates, and a rejected
 * setter leaves the configuration byte for byte as it was. A caller walking a
 * list of overrides can therefore stop at the first failure knowing it has not
 * left the configuration half applied.
 */

/*
 * Returns the length of `text`, capped at `limit`.
 *
 * The cap is what makes this safe on a fixed size field that may not be NUL
 * terminated: the result is always at most `limit`, so the caller can compare it
 * against `limit` to detect "filled to the end".
 *
 * strnlen would say the same thing, but it is POSIX rather than C17 and is not
 * declared under -std=c17. Four lines here beat a portability caveat.
 */
static size_t bounded_length(const char *text, size_t limit)
{
    size_t length = 0;

    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

/*
 * Validates a data directory path against the storage reserved for it.
 *
 * Shared by the setter and by astra_config_validate, so that a configuration
 * built by struct literal is held to exactly the same standard as one produced
 * through the API.
 */
static astra_status validate_data_dir(const char *path)
{
    size_t length;

    if (path == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    length = bounded_length(path, ASTRA_CONFIG_PATH_MAX);

    if (length == 0) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (length >= ASTRA_CONFIG_PATH_MAX) {
        /* Too long to store including the NUL. Reported as out of memory because
         * the path is valid; it is the buffer that is too small. */
        return ASTRA_ERR_OUT_OF_MEMORY;
    }

    return ASTRA_OK;
}

astra_status astra_config_init(astra_config *cfg)
{
    astra_status status;

    if (cfg == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    /*
     * Zeroed first so that no padding byte is ever observable, then filled field
     * by field. Assigning a whole initialiser value would be shorter, but it
     * silently leaves a newly added field at zero while the defaults say
     * otherwise; going through the setters means astra_config_init and
     * astra_config_validate can never disagree about what a valid config is.
     */
    memset(cfg, 0, sizeof *cfg);

    status = astra_config_set_data_dir(cfg, ASTRA_CONFIG_DEFAULT_DATA_DIR);
    if (status != ASTRA_OK) {
        return status;
    }

    cfg->log_level = ASTRA_CONFIG_DEFAULT_LOG_LEVEL;
    cfg->page_size = ASTRA_PAGE_SIZE_DEFAULT;

    return ASTRA_OK;
}

astra_status astra_config_validate(const astra_config *cfg)
{
    astra_status status;

    if (cfg == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    status = validate_data_dir(cfg->data_dir);
    if (status != ASTRA_OK) {
        return status;
    }

    if (cfg->log_level < ASTRA_LOG_TRACE || cfg->log_level >= ASTRA_LOG_LEVEL_COUNT) {
        return ASTRA_ERR_INVALID_STATE;
    }

    if (!astra_page_size_is_valid(cfg->page_size)) {
        return ASTRA_ERR_UNSUPPORTED;
    }

    return ASTRA_OK;
}

astra_status astra_config_set_data_dir(astra_config *cfg, const char *path)
{
    size_t length;
    astra_status status;

    if (cfg == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    status = validate_data_dir(path);
    if (status != ASTRA_OK) {
        return status;
    }

    length = strlen(path);
    memcpy(cfg->data_dir, path, length + 1u);

    return ASTRA_OK;
}

astra_status astra_config_set_log_level(astra_config *cfg, astra_log_level level)
{
    if (cfg == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (level < ASTRA_LOG_TRACE || level >= ASTRA_LOG_LEVEL_COUNT) {
        return ASTRA_ERR_INVALID_STATE;
    }

    cfg->log_level = level;
    return ASTRA_OK;
}

astra_status astra_config_set_page_size(astra_config *cfg, uint32 bytes)
{
    if (cfg == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (!astra_page_size_is_valid(bytes)) {
        return ASTRA_ERR_UNSUPPORTED;
    }

    cfg->page_size = bytes;
    return ASTRA_OK;
}

astra_status astra_config_copy(astra_config *dst, const astra_config *src)
{
    if (dst == NULL || src == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    *dst = *src;
    return ASTRA_OK;
}

int astra_config_describe(const astra_config *cfg, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }

    if (cfg == NULL) {
        return snprintf(out, out_size, "config: unset");
    }

    return snprintf(out, out_size, "data_dir=%s log_level=%s page_size=%lu",
                     cfg->data_dir,
                     astra_log_level_name(cfg->log_level),
                     (unsigned long)cfg->page_size);
}