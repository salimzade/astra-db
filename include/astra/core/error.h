/*
 * AstraDB core: structured errors.
 *
 * Design
 * ------
 * AstraDB has no exceptions. Every function that can fail returns an
 * `astra_status` value, and functions that want to describe *why* they failed
 * additionally fill in a caller owned `astra_error`.
 *
 * The design goal is that error reporting never allocates. An `astra_error`
 * holds borrowed pointers to static strings, so creating one on a failing path
 * cannot itself fail for lack of memory. That property matters a great deal in
 * a database, where allocation failure is itself a condition that has to be
 * reported.
 *
 * Ownership
 * ---------
 * - `astra_error` is always owned by the caller. It is a plain value, not a
 *   handle, and must be passed to `astra_error_reset` before reuse if the
 *   caller wants a defined starting state.
 * - `astra_error::subsystem` and `astra_error::message` are *borrowed*
 *   pointers to strings with static storage duration owned by the library. They
 *   are never freed, never modified and remain valid for the lifetime of the
 *   process.
 * - Every function in this header that returns `const char *` returns a static
 *   string owned by the library. The caller must not free or modify it.
 * - `astra_error_format` writes into caller supplied storage and allocates
 *   nothing.
 */
#ifndef ASTRA_CORE_ERROR_H
#define ASTRA_CORE_ERROR_H

#include "astra/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ---------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------------
 */

/**
 * Result of an API call.
 *
 * ASTRA_OK is the only success value; every other member is a failure. Values
 * are explicit rather than relying on the next member's value so that inserting
 * a new code in the middle can never renumber an existing one.
 *
 * The first two failure codes are the library lifecycle codes introduced with
 * the public API skeleton. Both belong to the invalid-state category.
 */
typedef enum astra_status {
    /** The call succeeded. */
    ASTRA_OK = 0,

    /** A parameter was NULL, out of range, or inconsistent with the others. */
    ASTRA_ERR_INVALID_ARGUMENT = 1,

    /** Memory could not be obtained, or a size calculation overflowed. */
    ASTRA_ERR_OUT_OF_MEMORY = 2,

    /** An operating system call or file operation failed. */
    ASTRA_ERR_IO = 3,

    /** The receiver is in the wrong state for the call, for example a double close. */
    ASTRA_ERR_INVALID_STATE = 4,

    /** A requested object does not exist. */
    ASTRA_ERR_NOT_FOUND = 5,

    /** An object with the requested identity already exists. */
    ASTRA_ERR_ALREADY_EXISTS = 6,

    /** On-disk or in-memory data failed an integrity check. */
    ASTRA_ERR_CORRUPTION = 7,

    /** The operation is recognised but not implemented by this build. */
    ASTRA_ERR_UNSUPPORTED = 8,

    /** An invariant was violated inside AstraDB. Always a bug in AstraDB. */
    ASTRA_ERR_INTERNAL = 9,

    /** astra_init() was called while the library is already initialized. */
    ASTRA_ERR_ALREADY_INITIALIZED = 10,

    /** astra_shutdown() was called while the library is not initialized. */
    ASTRA_ERR_NOT_INITIALIZED = 11,

    /** Number of members; not a valid status value. */
    ASTRA_STATUS_COUNT = 12
} astra_status;

/**
 * Error code, as returned by APIs.
 *
 * A synonym for `astra_status`, provided so that code which reasons about
 * failures rather than results can say what it means.
 */
typedef astra_status astra_error_code;

/** Number of distinct status values, excluding the ASTRA_STATUS_COUNT sentinel. */
#define ASTRA_STATUS_COUNT_TOTAL ((size_t)ASTRA_STATUS_COUNT)

/*
 * ---------------------------------------------------------------------------
 * Categories
 * ---------------------------------------------------------------------------
 */

/**
 * Coarse classification of an error.
 *
 * Categories group codes that callers may want to treat alike, for example to
 * translate a failure into an exit code or into a SQL error class. The mapping
 * from code to category is fixed and total.
 */
typedef enum astra_error_category {
    /** The call succeeded; the associated error is empty. */
    ASTRA_CATEGORY_SUCCESS = 0,

    /** Caller error: bad pointer, bad range, inconsistent arguments. */
    ASTRA_CATEGORY_INVALID_ARGUMENT = 1,

    /** Allocation failed or a size computation overflowed. */
    ASTRA_CATEGORY_OUT_OF_MEMORY = 2,

    /** Operating system or file level failure. */
    ASTRA_CATEGORY_IO = 3,

    /** Receiver is not in a state where the call is meaningful. */
    ASTRA_CATEGORY_INVALID_STATE = 4,

    /** The addressed object does not exist. */
    ASTRA_CATEGORY_NOT_FOUND = 5,

    /** The object being created already exists. */
    ASTRA_CATEGORY_ALREADY_EXISTS = 6,

    /** Data failed an integrity check. */
    ASTRA_CATEGORY_CORRUPTION = 7,

    /** Recognised but not implemented. */
    ASTRA_CATEGORY_UNSUPPORTED = 8,

    /** AstraDB internal invariant violated; a bug. */
    ASTRA_CATEGORY_INTERNAL = 9,

    /** Number of members; not a valid category value. */
    ASTRA_CATEGORY_COUNT = 10
} astra_error_category;

/** Number of distinct category values, excluding the ASTRA_CATEGORY_COUNT sentinel. */
#define ASTRA_CATEGORY_COUNT_TOTAL ((size_t)ASTRA_CATEGORY_COUNT)

/*
 * ---------------------------------------------------------------------------
 * The error value
 * ---------------------------------------------------------------------------
 */

/**
 * A described failure: code, subsystem and human readable message.
 *
 * Copyable by assignment. Contains no owning pointers, so copying is a plain
 * struct copy and never allocates.
 */
typedef struct astra_error {
    /** Machine readable error code. */
    astra_error_code code;

    /** Coarse classification of `code`. */
    astra_error_category category;

    /**
     * Name of the subsystem that produced the error, for example "log".
     *
     * Borrowed static string, owned by the library, never NULL for a non-empty
     * error.
     */
    const char *subsystem;

    /**
     * Human readable description of what went wrong.
     *
     * Borrowed static string, owned by the library, never NULL for a non-empty
     * error. For an error created without a specific message this holds the
     * generic description of `code`.
     */
    const char *message;
} astra_error;

/** An error in the "no failure" state: code ASTRA_OK, empty strings. */
#define ASTRA_ERROR_INIT { ASTRA_OK, ASTRA_CATEGORY_SUCCESS, "core", "success" }

/*
 * ---------------------------------------------------------------------------
 * Classification
 * ---------------------------------------------------------------------------
 */

/**
 * Returns the category of `status`.
 *
 * Parameters:
 *   status - any astra_status value. Out of range values are tolerated.
 *
 * Returns: the matching category, or ASTRA_CATEGORY_INTERNAL if `status` is not
 * a member of astra_status. Never allocates. Never fails.
 */
astra_error_category astra_status_category(astra_status status);

/**
 * Returns the symbolic name of `status`, for example "ASTRA_ERR_IO".
 *
 * Parameters:
 *   status - any astra_status value. Out of range values are tolerated.
 *
 * Returns: a static, NUL terminated string owned by the library. Valid for the
 * lifetime of the process; the caller must not free or modify it. Never NULL.
 * Never fails.
 */
const char *astra_status_name(astra_status status);

/**
 * Returns the generic description of `status`, for example "I/O error".
 *
 * This is the message an astra_error gets when the caller does not supply one.
 *
 * Parameters:
 *   status - any astra_status value. Out of range values are tolerated.
 *
 * Returns: a static, NUL terminated string owned by the library. Valid for the
 * lifetime of the process; the caller must not free or modify it. Never NULL.
 * Never fails.
 */
const char *astra_status_description(astra_status status);

/**
 * Returns the symbolic name of `category`, for example "ASTRA_CATEGORY_IO".
 *
 * Parameters:
 *   category - any astra_error_category value. Out of range values are
 *              tolerated.
 *
 * Returns: a static, NUL terminated string owned by the library. Valid for the
 * lifetime of the process; the caller must not free or modify it. Never NULL.
 * Never fails.
 */
const char *astra_error_category_name(astra_error_category category);

/**
 * Returns true when `status` is a member of astra_status.
 *
 * Intended for validating untrusted input before casting it to astra_status,
 * for example a value parsed from a configuration file.
 *
 * Parameters:
 *   status - any integer-like value; NULL is not applicable.
 *
 * Returns: true when ASTRA_OK <= status < ASTRA_STATUS_COUNT.
 * Never allocates. Never fails.
 */
bool astra_status_is_valid(astra_status status);

/*
 * ---------------------------------------------------------------------------
 * Creating, clearing and inspecting errors
 * ---------------------------------------------------------------------------
 */

/**
 * Writes a described failure into `out` and returns its code.
 *
 * This is shaped so that a failing function can report and propagate in one
 * statement:
 *
 *     return astra_error_make(err, ASTRA_ERR_IO, "file", "read failed");
 *
 * Parameters:
 *   out        - destination. Must not be NULL. Overwritten on success.
 *   code       - the failure code. Must be a member of astra_status and must
 *                not be ASTRA_OK; ASTRA_OK with a non-NULL `out` is treated as
 *                a caller error, see Errors.
 *   subsystem  - subsystem name, for example "log". NULL is allowed and selects
 *                the default name "core".
 *   message    - specific description. NULL is allowed and selects the generic
 *                description of `code`.
 *
 * Returns:
 *   `code` on success, so the value can be returned directly.
 *   ASTRA_ERR_INVALID_ARGUMENT if `out` is NULL, or if `code` is ASTRA_OK or
 *   outside the range of astra_status. In the latter case `out` is left
 *   unchanged.
 *
 * Ownership: nothing is allocated; `subsystem` and `message` are borrowed by
 * `out` and must remain valid. Passing string literals, as the API intends, is
 * always valid.
 */
astra_error_code astra_error_make(astra_error *out,
                                  astra_error_code code,
                                  const char *subsystem,
                                  const char *message);

/**
 * Resets `err` to the "no failure" state.
 *
 * Parameters:
 *   err - the error to clear. Must not be NULL; there is no way to report an
 *         error about clearing an error.
 *
 * Ownership: allocates nothing.
 */
void astra_error_reset(astra_error *err);

/**
 * Returns true when `err` holds no failure.
 *
 * Parameters:
 *   err - the error to inspect. NULL is allowed and yields true, so that
 *         "no error object" and "an empty error object" behave the same.
 *
 * Ownership: allocates nothing.
 */
bool astra_error_is_ok(const astra_error *err);

/**
 * Copies the contents of `src` into `dst`.
 *
 * Parameters:
 *   dst  - destination. Must not be NULL. Overwritten.
 *   src  - source. NULL is allowed and is equivalent to a reset `dst`.
 *
 * Ownership: allocates nothing. Because an astra_error holds only borrowed
 * pointers, the two values afterwards have identical lifetime semantics.
 */
void astra_error_copy(astra_error *dst, const astra_error *src);

/**
 * Writes a one line, human readable rendering of `err` into `out`.
 *
 * The rendering is of the form
 * `ASTRA_ERR_IO: I/O error [wal] write failed`.
 * It is intended for log messages and diagnostics, not for machine parsing.
 *
 * Parameters:
 *   err      - error to render. NULL is allowed and renders "ASTRA_OK".
 *   out      - destination buffer. May be NULL only when `out_size` is 0.
 *   out_size - capacity of `out` in bytes, including room for the terminating
 *              NUL. When 0 nothing is written.
 *
 * Returns:
 *   The number of bytes the full rendering would occupy, excluding the
 *   terminating NUL, following snprintf semantics. A return value greater than
 *   or equal to `out_size` means the output was truncated and is NUL
 *   terminated. A negative return means an encoding failure.
 *
 * Ownership: writes into caller owned storage and allocates nothing. On
 * truncation `out` remains a valid NUL terminated string.
 */
int astra_error_format(const astra_error *err, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_CORE_ERROR_H */