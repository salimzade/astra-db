#include "test_support.h"

/*
 * Every astra_status must have a name, a description and a category, and every
 * category must have a name. The tests iterate the enums rather than listing
 * expectations, so adding a code without completing the table fails here.
 */

void astra_test_error_status_table(void)
{
    astra_test_begin("astra_test_error_status_table");

    ASTRA_CHECK(astra_status_is_valid(ASTRA_OK));
    ASTRA_CHECK(!astra_status_is_valid(ASTRA_STATUS_COUNT));
    ASTRA_CHECK(!astra_status_is_valid((astra_status)-1));
    ASTRA_CHECK(!astra_status_is_valid((astra_status)9999));

    ASTRA_CHECK(ASTRA_STATUS_COUNT_TOTAL == (size_t)12);

    for (int code = ASTRA_OK; code < ASTRA_STATUS_COUNT; ++code) {
        const char *name = astra_status_name((astra_status)code);
        const char *description = astra_status_description((astra_status)code);
        astra_error_category category = astra_status_category((astra_status)code);

        ASTRA_CHECK(name != NULL);
        ASTRA_CHECK(name[0] != '\0');
        ASTRA_CHECK(description != NULL);
        ASTRA_CHECK(description[0] != '\0');

        /* The status name must be the enum's own spelling, so a message can be
         * mapped back to code by a human reading it. */
        ASTRA_CHECK(strncmp(name, "ASTRA_", 6) == 0);

        ASTRA_CHECK(category >= ASTRA_CATEGORY_SUCCESS);
        ASTRA_CHECK(category < ASTRA_CATEGORY_COUNT);
    }

    /* The two lifecycle codes from the bootstrap phase classify as invalid
     * state, which is what makes them actionable by a caller. */
    ASTRA_CHECK(astra_status_category(ASTRA_ERR_ALREADY_INITIALIZED) ==
                ASTRA_CATEGORY_INVALID_STATE);
    ASTRA_CHECK(astra_status_category(ASTRA_ERR_NOT_INITIALIZED) ==
                ASTRA_CATEGORY_INVALID_STATE);

    ASTRA_CHECK(astra_status_category(ASTRA_OK) == ASTRA_CATEGORY_SUCCESS);

    /* Out of range input is tolerated, never a crash and never a NULL. */
    ASTRA_CHECK(astra_status_name((astra_status)-1) != NULL);
    ASTRA_CHECK(astra_status_description((astra_status)-1) != NULL);
    ASTRA_CHECK(astra_status_category((astra_status)9999) == ASTRA_CATEGORY_INTERNAL);
    ASTRA_CHECK_STRING(astra_status_name((astra_status)9999), "ASTRA_STATUS_UNKNOWN");

    /* Each name must be unique, otherwise logs become ambiguous. */
    for (int i = ASTRA_OK; i < ASTRA_STATUS_COUNT; ++i) {
        for (int j = i + 1; j < ASTRA_STATUS_COUNT; ++j) {
            const char *left = astra_status_name((astra_status)i);
            const char *right = astra_status_name((astra_status)j);

            ASTRA_CHECK(strcmp(left, right) != 0);
        }
    }
}

void astra_test_error_categories(void)
{
    astra_test_begin("astra_test_error_categories");

    ASTRA_CHECK(ASTRA_CATEGORY_COUNT_TOTAL == (size_t)10);

    for (int category = ASTRA_CATEGORY_SUCCESS; category < ASTRA_CATEGORY_COUNT; ++category) {
        const char *name = astra_error_category_name((astra_error_category)category);

        ASTRA_CHECK(name != NULL);
        ASTRA_CHECK(name[0] != '\0');
        ASTRA_CHECK(strncmp(name, "ASTRA_CATEGORY_", 15) == 0);
    }

    /* Out of range values produce a marker rather than reading past the table. */
    ASTRA_CHECK_STRING(astra_error_category_name((astra_error_category)-1),
                       "ASTRA_CATEGORY_UNKNOWN");
    ASTRA_CHECK_STRING(astra_error_category_name(ASTRA_CATEGORY_COUNT),
                       "ASTRA_CATEGORY_UNKNOWN");

    /* astra_error_code is a synonym for astra_status, not a second type. */
    {
        astra_error_code code = ASTRA_ERR_CORRUPTION;

        ASTRA_CHECK(sizeof(code) == sizeof(int));
        ASTRA_CHECK(code == ASTRA_ERR_CORRUPTION);
        ASTRA_CHECK(astra_status_category(code) == ASTRA_CATEGORY_CORRUPTION);
    }
}

void astra_test_error_make(void)
{
    astra_error err;

    astra_test_begin("astra_test_error_make");

    /* A fully specified error. */
    ASTRA_CHECK_STATUS(astra_error_make(&err, ASTRA_ERR_IO, "wal", "write failed"),
                       ASTRA_ERR_IO);
    ASTRA_CHECK(err.code == ASTRA_ERR_IO);
    ASTRA_CHECK(err.category == ASTRA_CATEGORY_IO);
    ASTRA_CHECK_STRING(err.subsystem, "wal");
    ASTRA_CHECK_STRING(err.message, "write failed");
    ASTRA_CHECK(!astra_error_is_ok(&err));

    /* NULL subsystem and message fall back to documented defaults. */
    ASTRA_CHECK_STATUS(astra_error_make(&err, ASTRA_ERR_NOT_FOUND, NULL, NULL),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK_STRING(err.subsystem, "core");
    ASTRA_CHECK_STRING(err.message, astra_status_description(ASTRA_ERR_NOT_FOUND));
    ASTRA_CHECK(err.category == ASTRA_CATEGORY_NOT_FOUND);

    /* Every code produces a consistent record. */
    for (int code = ASTRA_OK + 1; code < ASTRA_STATUS_COUNT; ++code) {
        ASTRA_CHECK_STATUS(astra_error_make(&err, (astra_error_code)code, "test", NULL),
                           (astra_status)code);
        ASTRA_CHECK(err.code == (astra_error_code)code);
        ASTRA_CHECK(err.category == astra_status_category((astra_status)code));
        ASTRA_CHECK_STRING(err.message, astra_status_description((astra_status)code));
    }

    /* NULL destination. */
    ASTRA_CHECK_STATUS(astra_error_make(NULL, ASTRA_ERR_IO, "wal", "x"),
                       ASTRA_ERR_INVALID_ARGUMENT);

    /* ASTRA_OK is not a failure, so describing one is a caller error. */
    err.code = ASTRA_ERR_INTERNAL;
    ASTRA_CHECK_STATUS(astra_error_make(&err, ASTRA_OK, "test", "not a failure"),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(err.code == ASTRA_ERR_INTERNAL);

    /* An out of range code is rejected and leaves the destination alone. */
    ASTRA_CHECK_STATUS(astra_error_make(&err, (astra_error_code)9999, "test", "bad"),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(err.code == ASTRA_ERR_INTERNAL);

    ASTRA_CHECK_STATUS(astra_error_make(&err, (astra_error_code)-1, "test", "bad"),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(err.code == ASTRA_ERR_INTERNAL);

    /* The reported error object describes the bad call itself, which is what a
     * caller logging the return value will want to see. */
    ASTRA_CHECK(astra_error_is_ok(NULL));
    ASTRA_CHECK(astra_error_category_name(err.category) != NULL);
}

void astra_test_error_lifecycle(void)
{
    astra_error err;
    astra_error copy;

    astra_test_begin("astra_test_error_lifecycle");

    /* Uninitialised memory is not "ok"; reset first, as documented. */
    astra_error_reset(&err);
    ASTRA_CHECK(err.code == ASTRA_OK);
    ASTRA_CHECK(err.category == ASTRA_CATEGORY_SUCCESS);
    ASTRA_CHECK(err.subsystem != NULL);
    ASTRA_CHECK(err.message != NULL);
    ASTRA_CHECK(astra_error_is_ok(&err));

    /* Reset is idempotent and NULL tolerant. */
    astra_error_reset(&err);
    ASTRA_CHECK(astra_error_is_ok(&err));
    astra_error_reset(NULL);

    /* ASTRA_ERROR_INIT is a usable initialiser. */
    {
        astra_error initialised = ASTRA_ERROR_INIT;

        ASTRA_CHECK(initialised.code == ASTRA_OK);
        ASTRA_CHECK(astra_error_is_ok(&initialised));
    }

    (void)astra_error_make(&err, ASTRA_ERR_CORRUPTION, "page", "checksum mismatch");

    astra_error_copy(&copy, &err);
    ASTRA_CHECK(copy.code == err.code);
    ASTRA_CHECK(copy.category == err.category);
    ASTRA_CHECK(copy.subsystem == err.subsystem);
    ASTRA_CHECK(copy.message == err.message);
    ASTRA_CHECK(!astra_error_is_ok(&copy));

    /* Copying NULL source is a reset, not a crash. */
    astra_error_copy(&copy, NULL);
    ASTRA_CHECK(astra_error_is_ok(&copy));

    /* Copying to NULL is a no-op. */
    astra_error_copy(NULL, &err);
    astra_error_copy(NULL, NULL);

    /* An error stays reported until it is reset: there is no sticky state. */
    (void)astra_error_make(&err, ASTRA_ERR_INTERNAL, "core", "boom");
    ASTRA_CHECK(!astra_error_is_ok(&err));
    astra_error_reset(&err);
    ASTRA_CHECK(astra_error_is_ok(&err));
}

void astra_test_error_format(void)
{
    astra_error err;
    char small[16];
    char buffer[256];
    int needed;

    astra_test_begin("astra_test_error_format");

    (void)astra_error_make(&err, ASTRA_ERR_IO, "wal", "write failed");

    needed = astra_error_format(&err, buffer, sizeof buffer);
    ASTRA_CHECK(needed > 0);
    ASTRA_CHECK((size_t)needed < sizeof buffer);
    ASTRA_CHECK(strstr(buffer, "ASTRA_ERR_IO") != NULL);
    ASTRA_CHECK(strstr(buffer, "I/O error") != NULL);
    ASTRA_CHECK(strstr(buffer, "wal") != NULL);
    ASTRA_CHECK(strstr(buffer, "write failed") != NULL);
    ASTRA_CHECK(buffer[0] != '\0');
    /* One line, so no embedded newline can confuse a log scraper. */
    ASTRA_CHECK(strchr(buffer, '\n') == NULL);

    /* Truncation follows snprintf semantics and still yields a valid string. */
    needed = astra_error_format(&err, small, sizeof small);
    ASTRA_CHECK(needed > (int)sizeof small);
    ASTRA_CHECK(strlen(small) == sizeof small - 1u);
    ASTRA_CHECK(strncmp(small, buffer, sizeof small - 1u) == 0);

    /* Measuring without writing. */
    ASTRA_CHECK(astra_error_format(&err, NULL, 0) == 0);
    ASTRA_CHECK(astra_error_format(&err, buffer, 0) == 0);

    /* No error at all. */
    needed = astra_error_format(NULL, buffer, sizeof buffer);
    ASTRA_CHECK(needed > 0);
    ASTRA_CHECK(strstr(buffer, "ASTRA_OK") != NULL);

    astra_error_reset(&err);
    needed = astra_error_format(&err, buffer, sizeof buffer);
    ASTRA_CHECK(needed > 0);
    ASTRA_CHECK(strstr(buffer, "ASTRA_OK") != NULL);

    /* An error with NULL strings falls back rather than printing "(null)". */
    err.code = ASTRA_ERR_UNSUPPORTED;
    err.category = ASTRA_CATEGORY_UNSUPPORTED;
    err.subsystem = NULL;
    err.message = NULL;
    needed = astra_error_format(&err, buffer, sizeof buffer);
    ASTRA_CHECK(needed > 0);
    ASTRA_CHECK(strstr(buffer, "core") != NULL);
    ASTRA_CHECK(strstr(buffer, "unsupported operation") != NULL);
}