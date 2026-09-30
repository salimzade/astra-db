/*
 * Shared scaffolding for the AstraDB unit tests.
 *
 * The suite is a single executable that runs every group in-process and exits
 * non-zero if any check failed. That keeps it dependency free and lets the
 * allocator's live-block counters be checked across the whole run.
 */
#ifndef ASTRA_TESTS_TEST_SUPPORT_H
#define ASTRA_TESTS_TEST_SUPPORT_H

#include "astra/astra.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/** Checks that have failed so far in this run. */
extern unsigned int astra_test_failures;

/** Checks that have executed so far in this run. */
extern unsigned int astra_test_checks;

/** Prints the banner for the group about to run. */
void astra_test_begin(const char *group_name);

/** Records a failure for `group` at `line`. */
void astra_test_report_failure(const char *group, int line, const char *expression);

#define ASTRA_CHECK(condition)                                                       \
    do {                                                                             \
        ++astra_test_checks;                                                         \
        if (!(condition)) {                                                          \
            astra_test_report_failure(__func__, __LINE__, #condition);               \
        }                                                                            \
    } while (0)

/** Compares two statuses and reports both symbolically on failure. */
#define ASTRA_CHECK_STATUS(actual, expected)                                         \
    do {                                                                             \
        astra_status astra_actual_ = (actual);                                       \
        astra_status astra_expected_ = (expected);                                   \
        ++astra_test_checks;                                                         \
        if (astra_actual_ != astra_expected_) {                                      \
            astra_test_report_status(__func__, __LINE__, #actual,                    \
                                     astra_actual_, astra_expected_);                \
        }                                                                            \
    } while (0)

void astra_test_report_status(const char *group,
                              int line,
                              const char *expression,
                              astra_status actual,
                              astra_status expected);

/** Compares two unsigned values, printing them in hexadecimal on failure. */
#define ASTRA_CHECK_UINT64(actual, expected)                                         \
    do {                                                                             \
        uint64 astra_actual_ = (uint64)(actual);                                     \
        uint64 astra_expected_ = (uint64)(expected);                                 \
        ++astra_test_checks;                                                         \
        if (astra_actual_ != astra_expected_) {                                      \
            astra_test_report_uint64(__func__, __LINE__, #actual,                    \
                                     astra_actual_, astra_expected_);                \
        }                                                                            \
    } while (0)

void astra_test_report_uint64(const char *group,
                              int line,
                              const char *expression,
                              uint64 actual,
                              uint64 expected);

/** Compares two strings, reporting both on failure. NULL compares as NULL. */
#define ASTRA_CHECK_STRING(actual, expected)                                         \
    do {                                                                             \
        const char *astra_actual_ = (actual);                                        \
        const char *astra_expected_ = (expected);                                    \
        ++astra_test_checks;                                                         \
        if (!astra_strings_equal(astra_actual_, astra_expected_)) {                  \
            astra_test_report_string(__func__, __LINE__, #actual,                    \
                                     astra_actual_, astra_expected_);                \
        }                                                                            \
    } while (0)

/** NULL-safe string comparison; used only by the macro above. */
bool astra_strings_equal(const char *a, const char *b);

void astra_test_report_string(const char *group,
                              int line,
                              const char *expression,
                              const char *actual,
                              const char *expected);

/*
 * Test groups. Declared here so that every entry point has a visible prototype
 * before its definition, which the strict warning set requires.
 */

/* Library skeleton, from the bootstrap phase. */
void astra_test_version(void);
void astra_test_lifecycle(void);

/* astra/core/types.c */
void astra_test_type_widths(void);
void astra_test_type_predicates(void);
void astra_test_page_size_validity(void);

/* astra/core/error.c */
void astra_test_error_status_table(void);
void astra_test_error_categories(void);
void astra_test_error_make(void);
void astra_test_error_lifecycle(void);
void astra_test_error_format(void);

/* astra/core/allocator.c */
void astra_test_allocator_basics(void);
void astra_test_allocator_zeroed(void);
void astra_test_allocator_realloc(void);
void astra_test_allocator_strings(void);
void astra_test_allocator_tracking(void);
void astra_test_allocator_replacement(void);

/* astra/core/log.c */
void astra_test_log_level_names(void);
void astra_test_log_level_parse(void);
void astra_test_log_filtering(void);
void astra_test_log_rendering(void);
void astra_test_error_log(void);

/* astra/core/config.c */
void astra_test_config_defaults(void);
void astra_test_config_setters(void);
void astra_test_config_validate(void);
void astra_test_config_copy_and_describe(void);

#endif /* ASTRA_TESTS_TEST_SUPPORT_H */