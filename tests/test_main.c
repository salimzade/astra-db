#include "test_support.h"

#include <stdlib.h>

unsigned int astra_test_failures;
unsigned int astra_test_checks;

void astra_test_begin(const char *group_name)
{
    printf("%s\n", group_name);

    /*
     * Flush immediately. stdout is fully buffered when redirected, so a crash
     * part way through a run would otherwise discard every group that already
     * passed and leave no indication of where it died.
     */
    fflush(stdout);
}

void astra_test_report_failure(const char *group, int line, const char *expression)
{
    fprintf(stderr, "  FAIL %s:%d (%s): %s\n", group, line, __FILE__, expression);
    ++astra_test_failures;
}

void astra_test_report_status(const char *group,
                              int line,
                              const char *expression,
                              astra_status actual,
                              astra_status expected)
{
    fprintf(stderr, "  FAIL %s:%d (%s): %s was %s, expected %s\n",
            group, line, __FILE__, expression,
            astra_status_name(actual), astra_status_name(expected));
    ++astra_test_failures;
}

void astra_test_report_uint64(const char *group,
                              int line,
                              const char *expression,
                              uint64 actual,
                              uint64 expected)
{
    fprintf(stderr, "  FAIL %s:%d (%s): %s was 0x%016" PRIx64 ", expected 0x%016" PRIx64 "\n",
            group, line, __FILE__, expression, actual, expected);
    ++astra_test_failures;
}

bool astra_strings_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

void astra_test_report_string(const char *group,
                              int line,
                              const char *expression,
                              const char *actual,
                              const char *expected)
{
    fprintf(stderr, "  FAIL %s:%d (%s): %s was \"%s\", expected \"%s\"\n",
            group, line, __FILE__, expression,
            (actual != NULL) ? actual : "(null)",
            (expected != NULL) ? expected : "(null)");
    ++astra_test_failures;
}

static bool is_dotted_version(const char *text)
{
    size_t dots = 0;
    bool expect_digit = true;

    for (const char *p = text; *p != '\0'; ++p) {
        if (*p == '.') {
            if (expect_digit) {
                return false;
            }
            ++dots;
            expect_digit = true;
        } else if (*p >= '0' && *p <= '9') {
            expect_digit = false;
        } else {
            return false;
        }
    }

    return dots == 2 && !expect_digit;
}

void astra_test_version(void)
{
    const char *version;

    astra_test_begin("astra_test_version");

    version = astra_version();
    ASTRA_CHECK(version != NULL);
    if (version == NULL) {
        return;
    }

    ASTRA_CHECK(version[0] != '\0');
    ASTRA_CHECK(is_dotted_version(version));
    ASTRA_CHECK_STRING(version, ASTRA_DB_VERSION_STRING);
    ASTRA_CHECK(ASTRA_DB_NAME[0] != '\0');
}

void astra_test_lifecycle(void)
{
    astra_test_begin("astra_test_lifecycle");

    (void)astra_shutdown();

    ASTRA_CHECK_STATUS(astra_init(), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_init(), ASTRA_ERR_ALREADY_INITIALIZED);
    ASTRA_CHECK_STATUS(astra_shutdown(), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_shutdown(), ASTRA_ERR_NOT_INITIALIZED);
}

typedef void (*astra_test_fn)(void);

typedef struct astra_test_entry {
    const char *name;
    astra_test_fn run;
} astra_test_entry;

static const astra_test_entry k_tests[] = {
    { "version",       astra_test_version },

    { "type_widths",   astra_test_type_widths },
    { "type_predicates", astra_test_type_predicates },
    { "page_size_validity", astra_test_page_size_validity },

    { "error_status_table", astra_test_error_status_table },
    { "error_categories",   astra_test_error_categories },
    { "error_make",         astra_test_error_make },
    { "error_lifecycle",    astra_test_error_lifecycle },
    { "error_format",       astra_test_error_format },

    { "allocator_basics",   astra_test_allocator_basics },
    { "allocator_zeroed",   astra_test_allocator_zeroed },
    { "allocator_realloc",  astra_test_allocator_realloc },
    { "allocator_strings",  astra_test_allocator_strings },
    { "allocator_tracking", astra_test_allocator_tracking },
    { "allocator_replacement", astra_test_allocator_replacement },

    { "log_level_names",    astra_test_log_level_names },
    { "log_level_parse",    astra_test_log_level_parse },
    { "log_filtering",      astra_test_log_filtering },
    { "log_rendering",      astra_test_log_rendering },
    { "error_log",          astra_test_error_log },

    { "config_defaults",    astra_test_config_defaults },
    { "config_setters",     astra_test_config_setters },
    { "config_validate",    astra_test_config_validate },
    { "config_copy_and_describe", astra_test_config_copy_and_describe },

    { "lifecycle",     astra_test_lifecycle }
};

int main(void)
{
    const size_t count = sizeof k_tests / sizeof k_tests[0];

    for (size_t i = 0; i < count; ++i) {
        k_tests[i].run();
    }

    if (astra_test_failures != 0) {
        fprintf(stderr, "\n%u of %u check(s) failed in %zu group(s)\n",
                astra_test_failures, astra_test_checks, count);
        return EXIT_FAILURE;
    }

    printf("\nall %u checks passed in %zu group(s)\n", astra_test_checks, count);
    return EXIT_SUCCESS;
}