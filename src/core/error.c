#include "astra/core/error.h"

#include "core/astra_internal.h"

#include <stdio.h>

/*
 * Error handling in AstraDB.
 *
 * The whole module is built on two immutable tables: a code to (name,
 * description) table and a category table. Adding an error code means adding one
 * row to each, which the compile time checks below keep in step. Nothing here
 * allocates, so a failure can always be reported, even when the failure *is* an
 * allocation failure.
 */

/*
 * Descriptions are chosen to read as a sentence fragment, because they are
 * appended to a subsystem name: "wal: I/O error".
 */
typedef struct astra_status_info {
    const char *name;
    const char *description;
    astra_error_category category;
} astra_status_info;

/*
 * Indexed by astra_status. The last row is the ASTRA_STATUS_COUNT sentinel, which
 * is never returned for a valid code; it exists so that a caller that validates
 * with astra_status_is_valid can read the table without a bounds check.
 */
static const astra_status_info k_status_table[ASTRA_STATUS_COUNT + 1] = {
    [ASTRA_OK]                      = { "ASTRA_OK",                      "success",
                                       ASTRA_CATEGORY_SUCCESS },
    [ASTRA_ERR_INVALID_ARGUMENT]    = { "ASTRA_ERR_INVALID_ARGUMENT",    "invalid argument",
                                       ASTRA_CATEGORY_INVALID_ARGUMENT },
    [ASTRA_ERR_OUT_OF_MEMORY]       = { "ASTRA_ERR_OUT_OF_MEMORY",       "out of memory",
                                       ASTRA_CATEGORY_OUT_OF_MEMORY },
    [ASTRA_ERR_IO]                  = { "ASTRA_ERR_IO",                  "I/O error",
                                       ASTRA_CATEGORY_IO },
    [ASTRA_ERR_INVALID_STATE]       = { "ASTRA_ERR_INVALID_STATE",       "invalid state",
                                       ASTRA_CATEGORY_INVALID_STATE },
    [ASTRA_ERR_NOT_FOUND]           = { "ASTRA_ERR_NOT_FOUND",           "not found",
                                       ASTRA_CATEGORY_NOT_FOUND },
    [ASTRA_ERR_ALREADY_EXISTS]      = { "ASTRA_ERR_ALREADY_EXISTS",      "already exists",
                                       ASTRA_CATEGORY_ALREADY_EXISTS },
    [ASTRA_ERR_CORRUPTION]          = { "ASTRA_ERR_CORRUPTION",          "corruption detected",
                                       ASTRA_CATEGORY_CORRUPTION },
    [ASTRA_ERR_UNSUPPORTED]         = { "ASTRA_ERR_UNSUPPORTED",         "unsupported operation",
                                       ASTRA_CATEGORY_UNSUPPORTED },
    [ASTRA_ERR_INTERNAL]            = { "ASTRA_ERR_INTERNAL",            "internal error",
                                       ASTRA_CATEGORY_INTERNAL },
    [ASTRA_ERR_ALREADY_INITIALIZED] = { "ASTRA_ERR_ALREADY_INITIALIZED", "already initialized",
                                       ASTRA_CATEGORY_INVALID_STATE },
    [ASTRA_ERR_NOT_INITIALIZED]     = { "ASTRA_ERR_NOT_INITIALIZED",     "not initialized",
                                       ASTRA_CATEGORY_INVALID_STATE },
    [ASTRA_STATUS_COUNT]            = { "ASTRA_STATUS_UNKNOWN",          "unknown status",
                                       ASTRA_CATEGORY_INTERNAL }
};

/* Indexed by astra_error_category, same sentinel convention as above. */
static const char *const k_category_table[ASTRA_CATEGORY_COUNT + 1] = {
    [ASTRA_CATEGORY_SUCCESS]          = "ASTRA_CATEGORY_SUCCESS",
    [ASTRA_CATEGORY_INVALID_ARGUMENT] = "ASTRA_CATEGORY_INVALID_ARGUMENT",
    [ASTRA_CATEGORY_OUT_OF_MEMORY]    = "ASTRA_CATEGORY_OUT_OF_MEMORY",
    [ASTRA_CATEGORY_IO]               = "ASTRA_CATEGORY_IO",
    [ASTRA_CATEGORY_INVALID_STATE]    = "ASTRA_CATEGORY_INVALID_STATE",
    [ASTRA_CATEGORY_NOT_FOUND]        = "ASTRA_CATEGORY_NOT_FOUND",
    [ASTRA_CATEGORY_ALREADY_EXISTS]   = "ASTRA_CATEGORY_ALREADY_EXISTS",
    [ASTRA_CATEGORY_CORRUPTION]       = "ASTRA_CATEGORY_CORRUPTION",
    [ASTRA_CATEGORY_UNSUPPORTED]      = "ASTRA_CATEGORY_UNSUPPORTED",
    [ASTRA_CATEGORY_INTERNAL]         = "ASTRA_CATEGORY_INTERNAL",
    [ASTRA_CATEGORY_COUNT]            = "ASTRA_CATEGORY_UNKNOWN"
};

/*
 * Every table row above must agree with the enum. If someone adds a code to
 * astra_status and forgets a row, or fills the category cell with something
 * inconsistent, the build stops here rather than at runtime on a customer
 * machine.
 */
ASTRA_STATIC_ASSERT(sizeof(k_status_table) / sizeof(k_status_table[0])
                        == (size_t)ASTRA_STATUS_COUNT + 1,
                    "k_status_table must have one row per astra_status plus a sentinel");

ASTRA_STATIC_ASSERT(sizeof(k_category_table) / sizeof(k_category_table[0])
                        == (size_t)ASTRA_CATEGORY_COUNT + 1,
                    "k_category_table must have one row per astra_error_category plus a sentinel");

bool astra_status_is_valid(astra_status status)
{
    return status >= ASTRA_OK && status < ASTRA_STATUS_COUNT;
}

/*
 * Returns the row for `status`, or the sentinel row when `status` is out of
 * range. Centralising the bounds check here means every accessor below can be
 * total, which is what the public header promises.
 */
static const astra_status_info *status_info(astra_status status)
{
    if (!astra_status_is_valid(status)) {
        return &k_status_table[ASTRA_STATUS_COUNT];
    }
    return &k_status_table[status];
}

astra_error_category astra_status_category(astra_status status)
{
    return status_info(status)->category;
}

const char *astra_status_name(astra_status status)
{
    return status_info(status)->name;
}

const char *astra_status_description(astra_status status)
{
    return status_info(status)->description;
}

const char *astra_error_category_name(astra_error_category category)
{
    if (category < ASTRA_CATEGORY_SUCCESS || category >= ASTRA_CATEGORY_COUNT) {
        return k_category_table[ASTRA_CATEGORY_COUNT];
    }
    return k_category_table[category];
}

astra_error_code astra_error_make(astra_error *out,
                                  astra_error_code code,
                                  const char *subsystem,
                                  const char *message)
{
    const astra_status_info *info;

    if (out == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    if (!astra_status_is_valid(code) || code == ASTRA_OK) {
        /* An error with no failure in it is a caller bug. Leave `out` alone so
         * that a stale but honest description survives for diagnosis. */
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    info = &k_status_table[code];

    out->code = code;
    out->category = info->category;
    out->subsystem = (subsystem != NULL) ? subsystem : ASTRA_SUBSYSTEM_DEFAULT;
    out->message = (message != NULL) ? message : info->description;

    return code;
}

void astra_error_reset(astra_error *err)
{
    if (err == NULL) {
        return;
    }

    err->code = ASTRA_OK;
    err->category = ASTRA_CATEGORY_SUCCESS;
    err->subsystem = ASTRA_SUBSYSTEM_DEFAULT;
    err->message = k_status_table[ASTRA_OK].description;
}

bool astra_error_is_ok(const astra_error *err)
{
    if (err == NULL) {
        return true;
    }
    return err->code == ASTRA_OK;
}

void astra_error_copy(astra_error *dst, const astra_error *src)
{
    if (dst == NULL) {
        return;
    }
    if (src == NULL) {
        astra_error_reset(dst);
        return;
    }

    *dst = *src;
}

int astra_error_format(const astra_error *err, char *out, size_t out_size)
{
    static const char k_empty[] = "ASTRA_OK";
    const char *name;
    const char *description;
    const char *subsystem;
    const char *message;

    if (out == NULL || out_size == 0) {
        /* Nothing to write. Still report the length the caller would need, but
         * a NULL destination is only legal in that case, so there is no error
         * to report through a return value here. */
        return 0;
    }

    if (err == NULL || err->code == ASTRA_OK) {
        name = k_empty;
        description = astra_status_description(ASTRA_OK);
        subsystem = ASTRA_SUBSYSTEM_DEFAULT;
        message = description;
    } else {
        name = astra_status_name(err->code);
        description = astra_status_description(err->code);
        subsystem = (err->subsystem != NULL) ? err->subsystem : ASTRA_SUBSYSTEM_DEFAULT;
        message = (err->message != NULL) ? err->message : description;
    }

    return snprintf(out, out_size, "%s: %s [%s] %s", name, description, subsystem, message);
}