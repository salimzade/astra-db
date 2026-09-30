#include "astra/core/log.h"

#include "core/astra_internal.h"
#include "core/log_internal.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * AstraDB logging.
 *
 * Synchronous, one line per call, no queue. See the header for the output shape
 * and the ownership rules.
 *
 * Mutable state here is limited to two things: the minimum level, which is an
 * atomic because it is legitimately changed while the server is running, and the
 * sink, which is only ever changed by the test harness before concurrency
 * starts.
 */

/*
 * Width of the level field in the rendered line, e.g. "TRACE" or "INFO ".
 */
#define ASTRA_LOG_LEVEL_WIDTH 5

/*
 * Buffer for the rendered timestamp.
 *
 * The length is derived from the literal rather than written as a number. The
 * rendered stamp is "0000-00-00T00:00:00.000Z": 24 characters plus the
 * terminating NUL, and an off-by-one here silently truncates the trailing "Z",
 * which costs nothing at compile time and quietly corrupts every log line.
 */
#define ASTRA_LOG_TIMESTAMP_TEXT "0000-00-00T00:00:00.000Z"
#define ASTRA_LOG_TIMESTAMP_LEN (sizeof ASTRA_LOG_TIMESTAMP_TEXT)

static atomic_int g_min_level = ASTRA_LOG_INFO;

/*
 * The sink. Default writes to stderr; tests replace it. See log_internal.h for
 * why this is not part of the public API.
 */
static void default_sink(void *context, const char *line, size_t length)
{
    ASTRA_UNUSED(context);
    ASTRA_UNUSED(length);

    fputs(line, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

static astra_log_sink_fn g_sink = default_sink;
static void *g_sink_context;

/*
 * ---------------------------------------------------------------------------
 * Level table
 * ---------------------------------------------------------------------------
 */

/* Indexed by astra_log_level, including the ASTRA_LOG_NONE sentinel. */
static const char *const k_level_names[ASTRA_LOG_LEVEL_COUNT] = {
    [ASTRA_LOG_TRACE]      = "TRACE",
    [ASTRA_LOG_DEBUG]      = "DEBUG",
    [ASTRA_LOG_INFO]       = "INFO",
    [ASTRA_LOG_WARN]       = "WARN",
    [ASTRA_LOG_ERROR]      = "ERROR",
    [ASTRA_LOG_FATAL]      = "FATAL",
    [ASTRA_LOG_NONE]       = "NONE"
};

ASTRA_STATIC_ASSERT(ASTRA_ARRAY_LEN(k_level_names) == (size_t)ASTRA_LOG_LEVEL_COUNT,
                    "k_level_names must have one row per astra_log_level");

/*
 * ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 *
 * timespec_get with TIME_UTC is C11, so this needs no platform specific clock
 * call and no leap second handling. What does differ is the thread safe UTC
 * conversion, which POSIX spells gmtime_r and Windows spells gmtime_s. Only that
 * one call is conditional.
 */

/* Clamps `value` into [minimum, maximum]. */
static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

/*
 * Renders `seconds` since the epoch as ISO 8601 UTC into `out`.
 *
 * The stamp is formatted into a local buffer whose size is a compile time
 * constant, then copied out. Passing sizeof down as a runtime argument instead
 * leaves -Wformat-truncation unable to prove the result fits, and the warning is
 * worth silencing properly: the buffer is sized from the same literal the
 * format string is checked against, so it cannot be wrong.
 *
 * Returns false if the clock or the conversion failed.
 */
static bool format_timestamp(char *out, size_t out_size, long long seconds, int milliseconds)
{
    struct tm broken_down;
    time_t as_time_t = (time_t)seconds;
    char rendered[ASTRA_LOG_TIMESTAMP_LEN];
    int written;

#if defined(_WIN32)
    if (gmtime_s(&broken_down, &as_time_t) != 0) {
        return false;
    }
#else
    if (gmtime_r(&as_time_t, &broken_down) == NULL) {
        return false;
    }
#endif

    /*
     * Every field is clamped. The struct tm members are ints with no upper bound
     * the compiler can see, so without this -Wformat-truncation fires at -O3 and
     * a nonsensical clock would silently produce a ten digit year and truncate
     * the stamp.
     *
     * Formatted by hand rather than with strftime, because strftime output is
     * locale dependent and this format has to be stable across deployments.
     */
    written = snprintf(rendered, sizeof rendered, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                       clamp_int(broken_down.tm_year + 1900, 0, 9999),
                       clamp_int(broken_down.tm_mon + 1, 1, 12),
                       clamp_int(broken_down.tm_mday, 1, 31),
                       clamp_int(broken_down.tm_hour, 0, 23),
                       clamp_int(broken_down.tm_min, 0, 59),
                       clamp_int(broken_down.tm_sec, 0, 60),
                       clamp_int(milliseconds, 0, 999));

    if (written < 0 || (size_t)written >= sizeof rendered) {
        return false;
    }

    if (out_size > 0) {
        size_t copy = (size_t)written;

        if (copy >= out_size) {
            copy = out_size - 1u;
        }
        memcpy(out, rendered, copy);
        out[copy] = '\0';
    }

    return true;
}

/* Produces the current UTC timestamp. Falls back to a fixed marker if the clock fails. */
static void current_timestamp(char *out, size_t out_size)
{
    struct timespec now;

    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        snprintf(out, out_size, "0000-00-00T00:00:00.000Z");
        return;
    }

    if (!format_timestamp(out, out_size, (long long)now.tv_sec, (int)(now.tv_nsec / 1000000L))) {
        snprintf(out, out_size, "0000-00-00T00:00:00.000Z");
    }
}

/*
 * ---------------------------------------------------------------------------
 * Source location
 * ---------------------------------------------------------------------------
 */

/*
 * Returns the part of `path` after the last directory separator.
 *
 * Log lines are read by humans, and "astra_core.c:42" is easier to scan than
 * "/home/user/astra-db/src/core/astra_core.c:42". Both '/' and '\\' are accepted
 * so that a Windows path logged on either platform reads the same way.
 */
static const char *base_name(const char *path)
{
    const char *result = path;

    if (path == NULL) {
        return "?";
    }

    for (; *path != '\0'; ++path) {
        if (*path == '/' || *path == '\\') {
            result = path + 1;
        }
    }

    return result;
}

/*
 * ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 */

astra_status astra_log_set_level(astra_log_level level)
{
    if (level < ASTRA_LOG_TRACE || level >= ASTRA_LOG_LEVEL_COUNT) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    atomic_store_explicit(&g_min_level, (int)level, memory_order_relaxed);
    return ASTRA_OK;
}

astra_log_level astra_log_get_level(void)
{
    return (astra_log_level)atomic_load_explicit(&g_min_level, memory_order_relaxed);
}

bool astra_log_enabled(astra_log_level level)
{
    astra_log_level minimum = astra_log_get_level();

    if (minimum == ASTRA_LOG_NONE) {
        return false;
    }
    return level >= ASTRA_LOG_TRACE && level <= ASTRA_LOG_FATAL && level >= minimum;
}

const char *astra_log_level_name(astra_log_level level)
{
    if (level < ASTRA_LOG_TRACE || level >= ASTRA_LOG_LEVEL_COUNT) {
        return "UNKNOWN";
    }
    return k_level_names[level];
}

/* ASCII lowercase comparison; avoids depending on the non-standard strcasecmp. */
static bool equals_ignore_case(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char left = *a;
        char right = *b;

        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = (char)(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
        ++a;
        ++b;
    }

    return *a == '\0' && *b == '\0';
}

astra_status astra_log_level_parse(const char *text, astra_log_level *out)
{
    if (text == NULL || out == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    for (int level = ASTRA_LOG_TRACE; level < ASTRA_LOG_LEVEL_COUNT; ++level) {
        const char *name = k_level_names[level];

        if (equals_ignore_case(text, name)) {
            *out = (astra_log_level)level;
            return ASTRA_OK;
        }
    }

    /* "warning" is the word people actually type. */
    if (equals_ignore_case(text, "warning")) {
        *out = ASTRA_LOG_WARN;
        return ASTRA_OK;
    }
    if (equals_ignore_case(text, "off")) {
        *out = ASTRA_LOG_NONE;
        return ASTRA_OK;
    }

    return ASTRA_ERR_INVALID_ARGUMENT;
}

void astra_log_internal_set_sink(astra_log_sink_fn sink, void *context)
{
    if (sink == NULL) {
        g_sink = default_sink;
        g_sink_context = NULL;
        return;
    }

    g_sink = sink;
    g_sink_context = context;
}

bool astra_log_internal_has_custom_sink(void)
{
    return g_sink != default_sink;
}

/*
 * ---------------------------------------------------------------------------
 * Writing a record
 * ---------------------------------------------------------------------------
 */

void astra_log_write(astra_log_level level,
                     const char *file,
                     int line,
                     const char *subsystem,
                     const char *fmt,
                     ...)
{
    char buffer[ASTRA_LOG_LINE_MAX];
    char timestamp[ASTRA_LOG_TIMESTAMP_LEN];
    va_list arguments;
    size_t used;
    int written;

    /*
     * Filter first, format never. This is the whole reason a log level exists:
     * a disabled TRACE call in a hot loop must cost one atomic load.
     */
    if (!astra_log_enabled(level)) {
        return;
    }

    /* A corrupt level is still worth reporting, so it is promoted rather than
     * silently dropped. */
    if (level > ASTRA_LOG_FATAL) {
        level = ASTRA_LOG_ERROR;
    }

    if (fmt == NULL) {
        fmt = "";
    }

    current_timestamp(timestamp, sizeof timestamp);

    written = snprintf(buffer, sizeof buffer, "%s %-*s [%s] %s:%d: ",
                       timestamp,
                       ASTRA_LOG_LEVEL_WIDTH,
                       astra_log_level_name(level),
                       (subsystem != NULL) ? subsystem : ASTRA_SUBSYSTEM_DEFAULT,
                       base_name(file),
                       line);
    if (written < 0) {
        /* Encoding failure. Dropping the record is the only safe option. */
        return;
    }

    used = ((size_t)written < sizeof buffer) ? (size_t)written : (sizeof buffer - 1u);

    va_start(arguments, fmt);
    written = vsnprintf(buffer + used, sizeof buffer - used, fmt, arguments);
    va_end(arguments);

    if (written < 0) {
        return;
    }

    /* The prefix is not NUL terminated only when it was truncated, in which case
     * buffer is already fully populated and NUL terminated by snprintf. */
    g_sink(g_sink_context, buffer, strlen(buffer));
}

void astra_error_log(const astra_error *err)
{
    char rendered[ASTRA_LOG_LINE_MAX];

    if (err == NULL || err->code == ASTRA_OK) {
        return;
    }

    if (astra_error_format(err, rendered, sizeof rendered) < 0) {
        return;
    }

    ASTRA_LOG_ERROR((err->subsystem != NULL) ? err->subsystem : ASTRA_SUBSYSTEM_DEFAULT,
                    "%s", rendered);
}