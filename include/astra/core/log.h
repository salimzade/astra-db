/*
 * AstraDB core: logging.
 *
 * A deliberately small synchronous logger. One call writes one already formatted
 * line. There is no queue, no background thread, no formatting cache and no
 * level based buffering: those are added when a real need appears, not before.
 *
 * Output
 * ------
 * By default a line goes to stderr, which is what a database process wants:
 * stdout stays reserved for machine readable results. The exact shape is
 *
 *     2026-09-30T11:22:33.481Z INFO  [storage] src/core/page.c:120 message
 *     ^-- timestamp, UTC     ^-lvl ^-subsys  ^-source location           ^-text
 *
 * The timestamp is UTC in ISO 8601 with millisecond precision, so log output is
 * unambiguous regardless of the server's time zone and sorts lexicographically.
 *
 * Level filtering
 * ---------------
 * A single process wide minimum level is configured with
 * astra_log_set_level. A record below that level is discarded before any
 * formatting happens, so raising the level makes logging cheap. ASTRA_LOG_NONE
 * disables output entirely while keeping every call site compiled in.
 *
 * Ownership
 * ---------
 * - The `subsystem` and `fmt` arguments must outlive the call only. They are
 *   read during the call and not retained.
 * - Nothing is retained by the logger. There is no configuration object to
 *   destroy.
 * - Records are written synchronously before the logging call returns. A caller
 *   that writes to the same sink from several threads gets interleaved lines at
 *   the line level on POSIX where stderr is unbuffered, and may get
 *   interleaved fragments on platforms where it is not. Line atomicity is the
 *   only concurrency guarantee; see astra_log_set_level for the level itself.
 */
#ifndef ASTRA_CORE_LOG_H
#define ASTRA_CORE_LOG_H

#include "astra/core/error.h"
#include "astra/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Severity of a log record, ordered from most to least verbose. */
typedef enum astra_log_level {
    /** Very fine grained tracing; off in any normal build. */
    ASTRA_LOG_TRACE = 0,

    /** Diagnostic detail useful while debugging. */
    ASTRA_LOG_DEBUG = 1,

    /** Normal operational milestones: start up, shutdown, recovery. */
    ASTRA_LOG_INFO = 2,

    /** Something unexpected happened but the database is still correct. */
    ASTRA_LOG_WARN = 3,

    /** An operation failed. */
    ASTRA_LOG_ERROR = 4,

    /** Unrecoverable: the process is about to die. */
    ASTRA_LOG_FATAL = 5,

    /** Sentinel that suppresses every record. Not a record level itself. */
    ASTRA_LOG_NONE = 6,

    /**
     * Number of members of astra_log_level, including ASTRA_LOG_NONE.
     *
     * A usable as an array bound for a name table: every level, NONE included,
     * needs an entry. It is deliberately not the number of levels that produce
     * a record; that count excludes NONE, and is 6.
     */
    ASTRA_LOG_LEVEL_COUNT = 7
} astra_log_level;

/** Number of members of astra_log_level, including ASTRA_LOG_NONE. */
#define ASTRA_LOG_LEVEL_COUNT_TOTAL ((size_t)ASTRA_LOG_LEVEL_COUNT)
/** Longest formatted log line the logger will emit, excluding the NUL. */
#define ASTRA_LOG_LINE_MAX 4096

/*
 * Format string checking.
 *
 * Marking astra_log_write with the printf attribute makes the compiler validate
 * the arguments of every ASTRA_LOG_* macro at compile time, which is where the
 * value of this is realised. The macro is a no-op on compilers without the
 * attribute.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define ASTRA_LOG_PRINTF(format_index, first_arg) \
      __attribute__((format(printf, format_index, first_arg)))
#else
#  define ASTRA_LOG_PRINTF(format_index, first_arg)
#endif

/**
 * Writes one log record. Prefer the ASTRA_LOG_* macros.
 *
 * Parameters:
 *   level     - severity. A level outside the enum is treated as
 *               ASTRA_LOG_ERROR so that a corrupt level is still reported.
 *   file      - source file, normally __FILE__ from the macro. NULL is allowed
 *               and renders as "?".
 *   line      - source line, normally __LINE__ from the macro.
 *   subsystem - subsystem name, for example "log" or "wal". NULL is allowed and
 *               renders as "core".
 *   fmt       - printf style format string, followed by its arguments. Must not
 *               be NULL. Output longer than ASTRA_LOG_LINE_MAX is truncated.
 *
 * Returns: void. A logging failure is never reported to the caller: a logger
 * that cannot write must not turn into a second failure on the caller's error
 * path. Any write failure is silently dropped.
 *
 * Ownership: allocates nothing on the heap; the line is built in a stack buffer.
 * Neither `subsystem` nor `fmt` is retained.
 */
void astra_log_write(astra_log_level level,
                     const char *file,
                     int line,
                     const char *subsystem,
                     const char *fmt,
                     ...) ASTRA_LOG_PRINTF(5, 6);

/** Logs at ASTRA_LOG_TRACE. */
#define ASTRA_LOG_TRACE(subsystem, ...) \
    astra_log_write(ASTRA_LOG_TRACE, __FILE__, __LINE__, subsystem, __VA_ARGS__)

/** Logs at ASTRA_LOG_DEBUG. */
#define ASTRA_LOG_DEBUG(subsystem, ...) \
    astra_log_write(ASTRA_LOG_DEBUG, __FILE__, __LINE__, subsystem, __VA_ARGS__)

/** Logs at ASTRA_LOG_INFO. */
#define ASTRA_LOG_INFO(subsystem, ...) \
    astra_log_write(ASTRA_LOG_INFO, __FILE__, __LINE__, subsystem, __VA_ARGS__)

/** Logs at ASTRA_LOG_WARN. */
#define ASTRA_LOG_WARN(subsystem, ...) \
    astra_log_write(ASTRA_LOG_WARN, __FILE__, __LINE__, subsystem, __VA_ARGS__)

/** Logs at ASTRA_LOG_ERROR. */
#define ASTRA_LOG_ERROR(subsystem, ...) \
    astra_log_write(ASTRA_LOG_ERROR, __FILE__, __LINE__, subsystem, __VA_ARGS__)

/** Logs at ASTRA_LOG_FATAL. */
#define ASTRA_LOG_FATAL(subsystem, ...) \
    astra_log_write(ASTRA_LOG_FATAL, __FILE__, __LINE__, subsystem, __VA_ARGS__)

/*
 * ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 */

/**
 * Sets the minimum level that will be emitted.
 *
 * Process wide. Implemented with an atomic load and store, so it is safe to
 * call concurrently with logging from any thread.
 *
 * Parameters:
 *   level - new minimum. Must be a member of astra_log_level.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `level` is outside the enum; the previous
 *   level is left unchanged.
 *
 * Ownership: allocates nothing.
 */
astra_status astra_log_set_level(astra_log_level level);

/**
 * Returns the current minimum level.
 *
 * Never fails and allocates nothing. Defaults to ASTRA_LOG_INFO.
 */
astra_log_level astra_log_get_level(void);

/**
 * Returns true when a record at `level` would be emitted.
 *
 * Call this before assembling an expensive message.
 *
 * Parameters:
 *   level - severity to test. A level outside the enum yields false.
 *
 * Ownership: allocates nothing.
 */
bool astra_log_enabled(astra_log_level level);

/**
 * Returns the canonical uppercase name of `level`, for example "WARN".
 *
 * Parameters:
 *   level - any astra_log_level value; out of range values are tolerated.
 *
 * Returns: a static, NUL terminated string owned by the library, valid for the
 * lifetime of the process; the caller must not free or modify it. Never NULL.
 * Out of range inputs yield "UNKNOWN". Never fails.
 */
const char *astra_log_level_name(astra_log_level level);

/**
 * Parses a level name, case insensitively.
 *
 * Accepts "trace", "debug", "info", "warn", "warning", "error", "fatal", and
 * "none"/"off" for ASTRA_LOG_NONE.
 *
 * Parameters:
 *   text - NUL terminated name to parse. NULL is not allowed.
 *   out  - destination for the parsed level. Must not be NULL. Written only on
 *          success.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `text` or `out` is NULL, or if `text` is not a
 *   recognised name; `out` is then left unchanged.
 *
 * Ownership: allocates nothing.
 */
astra_status astra_log_level_parse(const char *text, astra_log_level *out);

/*
 * ---------------------------------------------------------------------------
 * Error integration
 * ---------------------------------------------------------------------------
 */

/**
 * Logs `err` at ERROR level, using the subsystem recorded in `err`.
 *
 * The message is the rendering produced by astra_error_format. Does nothing
 * when `err` is NULL or holds ASTRA_OK, so it is safe to place on the failure
 * path of any function.
 *
 * Parameters:
 *   err - error to report. NULL is allowed.
 *
 * Ownership: allocates nothing.
 */
void astra_error_log(const astra_error *err);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_CORE_LOG_H */