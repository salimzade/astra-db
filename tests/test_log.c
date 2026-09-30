#include "test_support.h"

#include "core/log_internal.h"

/*
 * Log output is captured by installing a sink through the private seam in
 * log_internal.h rather than by reading stderr. That is the reason the seam
 * exists: a test that cannot see the output cannot prove that filtering works.
 *
 * The capture buffer is file scoped because it is 16 lines of 4 KiB, which would
 * be a large stack object, and because the test suite is single threaded.
 */
#define ASTRA_TEST_LOG_LINES 16

typedef struct log_capture {
    size_t count;
    char lines[ASTRA_TEST_LOG_LINES][ASTRA_LOG_LINE_MAX];
    size_t lengths[ASTRA_TEST_LOG_LINES];
    bool truncated;
} log_capture;

static log_capture g_capture;

static void capture_sink(void *context, const char *line, size_t length)
{
    log_capture *capture = (log_capture *)context;
    size_t copy;

    if (capture->count >= ASTRA_TEST_LOG_LINES) {
        capture->truncated = true;
        return;
    }

    copy = (length < ASTRA_LOG_LINE_MAX) ? length : (ASTRA_LOG_LINE_MAX - 1u);
    memcpy(capture->lines[capture->count], line, copy);
    capture->lines[capture->count][copy] = '\0';
    capture->lengths[capture->count] = length;
    ++capture->count;
}

/* Installs the capture sink and clears it. */
static void begin_capture(void)
{
    memset(&g_capture, 0, sizeof g_capture);
    astra_log_internal_set_sink(capture_sink, &g_capture);
}

static void end_capture(void)
{
    astra_log_internal_set_sink(NULL, NULL);
}

/*
 * Checks the timestamp prefix shape, one character class at a time.
 *
 * The logger promises a fixed width UTC stamp of the form
 *   YYYY-MM-DDThh:mm:ss.mmmZ
 * so that log lines sort lexicographically. Verifying the character classes,
 * rather than comparing against a captured golden string, keeps the check from
 * being a tautology and still catches a dropped field or a missing zero pad.
 */
static bool has_timestamp_prefix(const char *line)
{
    /* "2026-09-30T11:22:33.481Z" */
    static const char k_expected[] = "2026-09-30T11:22:33.481Z";
    static const size_t k_digit_positions[] = { 0, 1, 2, 3, 5, 6, 8, 9,
                                                 11, 12, 14, 15, 17, 18, 20, 21, 22 };
    static const size_t k_fixed_positions[] = { 4, 7, 10, 13, 16, 19, 23 };
    static const char k_fixed_characters[] = { '-', '-', 'T', ':', ':', '.', 'Z' };

    if (line == NULL) {
        return false;
    }

    /* The timestamp is 24 characters and must be followed by a separator. */
    if (strlen(line) < sizeof k_expected) {
        return false;
    }

    for (size_t i = 0; i < ASTRA_ARRAY_LEN(k_digit_positions); ++i) {
        const char character = line[k_digit_positions[i]];

        if (character < '0' || character > '9') {
            return false;
        }
    }

    for (size_t i = 0; i < ASTRA_ARRAY_LEN(k_fixed_positions); ++i) {
        if (line[k_fixed_positions[i]] != k_fixed_characters[i]) {
            return false;
        }
    }

    /* The separator between the timestamp and the level. */
    return line[sizeof k_expected - 1u] == ' ';
}

static const char *captured_line(size_t index)
{
    if (index >= g_capture.count) {
        return NULL;
    }
    return g_capture.lines[index];
}

void astra_test_log_level_names(void)
{
    astra_test_begin("astra_test_log_level_names");

    ASTRA_CHECK_STRING(astra_log_level_name(ASTRA_LOG_TRACE), "TRACE");
    ASTRA_CHECK_STRING(astra_log_level_name(ASTRA_LOG_DEBUG), "DEBUG");
    ASTRA_CHECK_STRING(astra_log_level_name(ASTRA_LOG_INFO), "INFO");
    ASTRA_CHECK_STRING(astra_log_level_name(ASTRA_LOG_WARN), "WARN");
    ASTRA_CHECK_STRING(astra_log_level_name(ASTRA_LOG_ERROR), "ERROR");
    ASTRA_CHECK_STRING(astra_log_level_name(ASTRA_LOG_FATAL), "FATAL");
    ASTRA_CHECK_STRING(astra_log_level_name(ASTRA_LOG_NONE), "NONE");

    /* Out of range values must not read past the table. */
    ASTRA_CHECK_STRING(astra_log_level_name((astra_log_level)-1), "UNKNOWN");
    ASTRA_CHECK_STRING(astra_log_level_name(ASTRA_LOG_LEVEL_COUNT), "UNKNOWN");

    /* The configuration and logging modules must agree on the level count. */
    ASTRA_CHECK(ASTRA_LOG_LEVEL_COUNT_TOTAL == (size_t)7);
}

void astra_test_log_level_parse(void)
{
    astra_log_level level;

    astra_test_begin("astra_test_log_level_parse");

    ASTRA_CHECK_STATUS(astra_log_level_parse("trace", &level), ASTRA_OK);
    ASTRA_CHECK(level == ASTRA_LOG_TRACE);

    ASTRA_CHECK_STATUS(astra_log_level_parse("DEBUG", &level), ASTRA_OK);
    ASTRA_CHECK(level == ASTRA_LOG_DEBUG);

    /* Case insensitivity, because configuration comes from humans. */
    ASTRA_CHECK_STATUS(astra_log_level_parse("WaRn", &level), ASTRA_OK);
    ASTRA_CHECK(level == ASTRA_LOG_WARN);

    ASTRA_CHECK_STATUS(astra_log_level_parse("warning", &level), ASTRA_OK);
    ASTRA_CHECK(level == ASTRA_LOG_WARN);

    ASTRA_CHECK_STATUS(astra_log_level_parse("fatal", &level), ASTRA_OK);
    ASTRA_CHECK(level == ASTRA_LOG_FATAL);

    ASTRA_CHECK_STATUS(astra_log_level_parse("none", &level), ASTRA_OK);
    ASTRA_CHECK(level == ASTRA_LOG_NONE);

    ASTRA_CHECK_STATUS(astra_log_level_parse("OFF", &level), ASTRA_OK);
    ASTRA_CHECK(level == ASTRA_LOG_NONE);

    /* A rejected name must leave the destination alone. */
    level = ASTRA_LOG_TRACE;
    ASTRA_CHECK_STATUS(astra_log_level_parse("verbose", &level), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(level == ASTRA_LOG_TRACE);

    ASTRA_CHECK_STATUS(astra_log_level_parse("", &level), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_log_level_parse("warnin", &level), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_log_level_parse("warn ", &level), ASTRA_ERR_INVALID_ARGUMENT);

    /* NULL arguments are refused, not dereferenced. */
    ASTRA_CHECK_STATUS(astra_log_level_parse(NULL, &level), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_log_level_parse("info", NULL), ASTRA_ERR_INVALID_ARGUMENT);

    /* Every canonical name round trips. */
    for (int value = ASTRA_LOG_TRACE; value < ASTRA_LOG_LEVEL_COUNT; ++value) {
        astra_log_level parsed;

        ASTRA_CHECK_STATUS(astra_log_level_parse(astra_log_level_name((astra_log_level)value),
                                                &parsed),
                           ASTRA_OK);
        ASTRA_CHECK(parsed == (astra_log_level)value);
    }
}

void astra_test_log_filtering(void)
{
    astra_log_level previous;

    astra_test_begin("astra_test_log_filtering");

    previous = astra_log_get_level();

    /* Setting the level is total over the enum and validated outside it. */
    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_TRACE), ASTRA_OK);
    ASTRA_CHECK(astra_log_get_level() == ASTRA_LOG_TRACE);

    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_NONE), ASTRA_OK);
    ASTRA_CHECK(astra_log_get_level() == ASTRA_LOG_NONE);

    /* An invalid level leaves the previous one in place. */
    ASTRA_CHECK_STATUS(astra_log_set_level((astra_log_level)99), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(astra_log_get_level() == ASTRA_LOG_NONE);

    /* ASTRA_LOG_NONE silences everything, and so does any invalid level. */
    ASTRA_CHECK(!astra_log_enabled(ASTRA_LOG_TRACE));
    ASTRA_CHECK(!astra_log_enabled(ASTRA_LOG_FATAL));
    ASTRA_CHECK(!astra_log_enabled(ASTRA_LOG_ERROR));
    ASTRA_CHECK(!astra_log_enabled((astra_log_level)-1));

    /* A threshold admits its own level and everything more severe. */
    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_WARN), ASTRA_OK);
    ASTRA_CHECK(!astra_log_enabled(ASTRA_LOG_TRACE));
    ASTRA_CHECK(!astra_log_enabled(ASTRA_LOG_DEBUG));
    ASTRA_CHECK(!astra_log_enabled(ASTRA_LOG_INFO));
    ASTRA_CHECK(astra_log_enabled(ASTRA_LOG_WARN));
    ASTRA_CHECK(astra_log_enabled(ASTRA_LOG_ERROR));
    ASTRA_CHECK(astra_log_enabled(ASTRA_LOG_FATAL));
    ASTRA_CHECK(!astra_log_enabled((astra_log_level)-1));
    ASTRA_CHECK(!astra_log_enabled(ASTRA_LOG_NONE));

    /* TRACE admits everything. */
    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_TRACE), ASTRA_OK);
    ASTRA_CHECK(astra_log_enabled(ASTRA_LOG_TRACE));

    /* And nothing survives above the highest record level. */
    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_FATAL), ASTRA_OK);
    ASTRA_CHECK(astra_log_enabled(ASTRA_LOG_FATAL));
    ASTRA_CHECK(!astra_log_enabled(ASTRA_LOG_ERROR));

    /* Now prove the filter reaches the sink, not just the predicate. */
    begin_capture();

    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_WARN), ASTRA_OK);

    ASTRA_LOG_INFO("filtering", "this must be dropped");
    ASTRA_LOG_WARN("filtering", "this must survive");
    ASTRA_LOG_ERROR("filtering", "this must also survive");

    end_capture();

    ASTRA_CHECK(g_capture.count == 2u);
    ASTRA_CHECK(!g_capture.truncated);
    if (g_capture.count == 2u) {
        ASTRA_CHECK(strstr(captured_line(0), "WARN") != NULL);
        ASTRA_CHECK(strstr(captured_line(0), "this must survive") != NULL);
        ASTRA_CHECK(strstr(captured_line(0), "this must be dropped") == NULL);

        ASTRA_CHECK(strstr(captured_line(1), "ERROR") != NULL);
        ASTRA_CHECK(strstr(captured_line(1), "this must also survive") != NULL);
    }

    /* ASTRA_LOG_NONE reaches the sink as silence too. */
    begin_capture();
    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_NONE), ASTRA_OK);
    ASTRA_LOG_FATAL("filtering", "even this is dropped");
    end_capture();

    ASTRA_CHECK(g_capture.count == 0u);

    /* Leave the level as it was found, so the tests do not depend on order. */
    ASTRA_CHECK_STATUS(astra_log_set_level(previous), ASTRA_OK);
}

void astra_test_log_rendering(void)
{
    astra_log_level previous;

    astra_test_begin("astra_test_log_rendering");

    previous = astra_log_get_level();
    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_TRACE), ASTRA_OK);

    begin_capture();

    /* A record with all four documented components. */
    ASTRA_LOG_INFO("storage", "opened data_dir=%s pages=%lu", "data", 4096UL);

    /* The message is rendered through printf formatting, and the compiler's
     * format checking is what keeps this honest. */
    ASTRA_LOG_DEBUG("storage", "value=%d name=%s", -7, "seven");

    /* A message with no arguments at all. */
    ASTRA_LOG_TRACE("storage", "no arguments here");

    /* Missing optional components must still produce a usable line. */
    {
        const char *no_subsystem = NULL;
        const char *no_file = NULL;

        astra_log_write(ASTRA_LOG_ERROR, no_file, 0, no_subsystem, "defaults applied");
    }

    end_capture();

    ASTRA_CHECK(g_capture.count == 4u);
    ASTRA_CHECK(!g_capture.truncated);

    if (g_capture.count == 4u) {
        const char *first = captured_line(0);

        /* Timestamp. */
        ASTRA_CHECK(has_timestamp_prefix(first));

        /* Level. */
        ASTRA_CHECK(strstr(first, "INFO") != NULL);

        /* Subsystem. */
        ASTRA_CHECK(strstr(first, "[storage]") != NULL);

        /* Source location. */
        ASTRA_CHECK(strstr(first, "test_log.c:") != NULL);

        /* Message, fully formatted. */
        ASTRA_CHECK(strstr(first, "opened data_dir=data pages=4096") != NULL);

        /* One record, one line. */
        ASTRA_CHECK(strchr(first, '\n') == NULL);
        ASTRA_CHECK(strlen(first) == g_capture.lengths[0]);
        ASTRA_CHECK(g_capture.lengths[0] < ASTRA_LOG_LINE_MAX);

        /* printf formatting is applied, and %s of an int is not silently
         * reinterpreted. */
        ASTRA_CHECK(strstr(captured_line(1), "value=-7 name=seven") != NULL);

        ASTRA_CHECK(strstr(captured_line(2), "no arguments here") != NULL);

        /* The default subsystem name is applied. */
        ASTRA_CHECK(strstr(captured_line(3), "[core]") != NULL);
        ASTRA_CHECK(strstr(captured_line(3), "defaults applied") != NULL);
    }

    /* Overlong messages are truncated, not dropped, and never overflow. */
    begin_capture();
    {
        char huge[ASTRA_LOG_LINE_MAX * 2];

        memset(huge, 'x', sizeof huge - 1u);
        huge[sizeof huge - 1u] = '\0';
        ASTRA_LOG_INFO("storage", "%s", huge);
    }
    end_capture();

    ASTRA_CHECK(g_capture.count == 1u);
    if (g_capture.count == 1u) {
        ASTRA_CHECK(g_capture.lengths[0] < ASTRA_LOG_LINE_MAX);
        ASTRA_CHECK(strlen(captured_line(0)) == g_capture.lengths[0]);
    }

    /* Levels are rendered under their own names. */
    begin_capture();
    ASTRA_LOG_FATAL("storage", "fatal record");
    end_capture();

    ASTRA_CHECK(g_capture.count == 1u);
    if (g_capture.count == 1u) {
        ASTRA_CHECK(strstr(captured_line(0), "FATAL") != NULL);
        ASTRA_CHECK(strstr(captured_line(0), "fatal record") != NULL);
    }

    ASTRA_CHECK_STATUS(astra_log_set_level(previous), ASTRA_OK);

    /* The seam reports whether it is customised, so tests can tell that they are
     * the ones redirecting output. */
    ASTRA_CHECK(!astra_log_internal_has_custom_sink());
    begin_capture();
    ASTRA_CHECK(astra_log_internal_has_custom_sink());
    end_capture();
    ASTRA_CHECK(!astra_log_internal_has_custom_sink());
}

void astra_test_error_log(void)
{
    astra_error err;
    astra_log_level previous;

    astra_test_begin("astra_test_error_log");

    previous = astra_log_get_level();
    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_TRACE), ASTRA_OK);

    /* No error and no object at all: nothing to report, so nothing is written. */
    begin_capture();
    astra_error_log(NULL);
    astra_error_reset(&err);
    astra_error_log(&err);
    end_capture();
    ASTRA_CHECK(g_capture.count == 0u);

    /* A described error is reported at ERROR level under its own subsystem. */
    begin_capture();
    (void)astra_error_make(&err, ASTRA_ERR_CORRUPTION, "page", "checksum mismatch");
    astra_error_log(&err);
    end_capture();

    ASTRA_CHECK(g_capture.count == 1u);
    if (g_capture.count == 1u) {
        const char *line = captured_line(0);

        ASTRA_CHECK(has_timestamp_prefix(line));
        ASTRA_CHECK(strstr(line, "ERROR") != NULL);
        ASTRA_CHECK(strstr(line, "[page]") != NULL);
        ASTRA_CHECK(strstr(line, "ASTRA_ERR_CORRUPTION") != NULL);
        ASTRA_CHECK(strstr(line, "checksum mismatch") != NULL);
    }

    /* Error logging honours the level, exactly like a direct record. */
    begin_capture();
    ASTRA_CHECK_STATUS(astra_log_set_level(ASTRA_LOG_FATAL), ASTRA_OK);
    astra_error_log(&err);
    end_capture();
    ASTRA_CHECK(g_capture.count == 0u);

    ASTRA_CHECK_STATUS(astra_log_set_level(previous), ASTRA_OK);
}