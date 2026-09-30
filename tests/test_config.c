#include "test_support.h"

void astra_test_config_defaults(void)
{
    astra_config config;

    astra_test_begin("astra_test_config_defaults");

    ASTRA_CHECK_STATUS(astra_config_init(&config), ASTRA_OK);

    /* The documented defaults. If these ever change, the macros and the
     * implementation must change together, and this is where it shows. */
    ASTRA_CHECK_STRING(config.data_dir, ASTRA_CONFIG_DEFAULT_DATA_DIR);
    ASTRA_CHECK_STRING(config.data_dir, "data");
    ASTRA_CHECK(config.log_level == ASTRA_LOG_INFO);
    ASTRA_CHECK(config.log_level == ASTRA_CONFIG_DEFAULT_LOG_LEVEL);
    ASTRA_CHECK(config.page_size == 4096u);
    ASTRA_CHECK(config.page_size == ASTRA_PAGE_SIZE_DEFAULT);

    /* The defaults must satisfy the validation the configuration promises. */
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_OK);

    /* The path is always NUL terminated, so no caller has to bounds check. */
    ASTRA_CHECK(strlen(config.data_dir) < ASTRA_CONFIG_PATH_MAX);
    ASTRA_CHECK(config.data_dir[ASTRA_CONFIG_PATH_MAX - 1] == '\0');

    /* Init is idempotent, so re-initialising a used struct restores the
     * defaults rather than merging into whatever was there. */
    ASTRA_CHECK_STATUS(astra_config_set_page_size(&config, 16384u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_config_set_log_level(&config, ASTRA_LOG_FATAL), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_config_init(&config), ASTRA_OK);
    ASTRA_CHECK(config.page_size == 4096u);
    ASTRA_CHECK(config.log_level == ASTRA_LOG_INFO);

    /* A NULL destination is reported, not dereferenced. */
    ASTRA_CHECK_STATUS(astra_config_init(NULL), ASTRA_ERR_INVALID_ARGUMENT);
}

void astra_test_config_setters(void)
{
    astra_config config;
    char exact[ASTRA_CONFIG_PATH_MAX];
    char too_long[ASTRA_CONFIG_PATH_MAX + 8];

    astra_test_begin("astra_test_config_setters");

    ASTRA_CHECK_STATUS(astra_config_init(&config), ASTRA_OK);

    /* Data directory. */
    ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, "/var/lib/astradb"), ASTRA_OK);
    ASTRA_CHECK_STRING(config.data_dir, "/var/lib/astradb");
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_OK);

    /* The caller's buffer is copied, not borrowed. */
    {
        char scratch[] = "/scratch";

        ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, scratch), ASTRA_OK);
        scratch[1] = 'X';
        ASTRA_CHECK_STRING(config.data_dir, "/scratch");
    }

    /* The longest path that fits, including the NUL. */
    memset(exact, 'd', sizeof exact - 1u);
    exact[sizeof exact - 1u] = '\0';
    ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, exact), ASTRA_OK);
    ASTRA_CHECK(strlen(config.data_dir) == ASTRA_CONFIG_PATH_MAX - 1u);

    /* One byte more does not fit. */
    memset(too_long, 'd', sizeof too_long - 1u);
    too_long[sizeof too_long - 1u] = '\0';
    ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, too_long),
                       ASTRA_ERR_OUT_OF_MEMORY);
    /* A rejected setter changes nothing. */
    ASTRA_CHECK_STRING(config.data_dir, exact);

    ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, NULL), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, ""), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_config_set_data_dir(NULL, "data"), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STRING(config.data_dir, exact);

    /* Log level. */
    ASTRA_CHECK_STATUS(astra_config_set_log_level(&config, ASTRA_LOG_TRACE), ASTRA_OK);
    ASTRA_CHECK(config.log_level == ASTRA_LOG_TRACE);
    ASTRA_CHECK_STATUS(astra_config_set_log_level(&config, ASTRA_LOG_NONE), ASTRA_OK);
    ASTRA_CHECK(config.log_level == ASTRA_LOG_NONE);
    ASTRA_CHECK_STATUS(astra_config_set_log_level(&config, (astra_log_level)-1),
                       ASTRA_ERR_INVALID_STATE);
    ASTRA_CHECK_STATUS(astra_config_set_log_level(&config, ASTRA_LOG_LEVEL_COUNT),
                       ASTRA_ERR_INVALID_STATE);
    ASTRA_CHECK_STATUS(astra_config_set_log_level(&config, 12345), ASTRA_ERR_INVALID_STATE);
    ASTRA_CHECK_STATUS(astra_config_set_log_level(NULL, ASTRA_LOG_INFO),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(config.log_level == ASTRA_LOG_NONE);

    /* Page size. */
    ASTRA_CHECK_STATUS(astra_config_set_page_size(&config, ASTRA_PAGE_SIZE_MIN), ASTRA_OK);
    ASTRA_CHECK(config.page_size == ASTRA_PAGE_SIZE_MIN);
    ASTRA_CHECK_STATUS(astra_config_set_page_size(&config, ASTRA_PAGE_SIZE_MAX), ASTRA_OK);
    ASTRA_CHECK(config.page_size == ASTRA_PAGE_SIZE_MAX);
    ASTRA_CHECK_STATUS(astra_config_set_page_size(&config, 8192u), ASTRA_OK);
    ASTRA_CHECK(config.page_size == 8192u);

    ASTRA_CHECK_STATUS(astra_config_set_page_size(&config, 0u), ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(astra_config_set_page_size(&config, ASTRA_PAGE_SIZE_MIN - 1u),
                       ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(astra_config_set_page_size(&config, ASTRA_PAGE_SIZE_MAX + 1u),
                       ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(astra_config_set_page_size(&config, UINT32_MAX), ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(astra_config_set_page_size(NULL, 4096u), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(config.page_size == 8192u);
}

void astra_test_config_validate(void)
{
    astra_config config;

    astra_test_begin("astra_test_config_validate");

    ASTRA_CHECK_STATUS(astra_config_init(&config), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_config_validate(NULL), ASTRA_ERR_INVALID_ARGUMENT);

    /*
     * Validation also covers configurations that never passed through a setter,
     * which is the case that matters: a struct built by a literal or deserialised
     * from a file must be held to the same standard.
     */

    /* An empty data directory is rejected. */
    config.data_dir[0] = '\0';
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_ERR_INVALID_ARGUMENT);

    /* So is a data directory with no terminator anywhere in the array, which
     * only happens if the struct was corrupted. */
    memset(config.data_dir, 'x', sizeof config.data_dir);
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_ERR_OUT_OF_MEMORY);

    /* And one that fills the array exactly, terminator included, is still
     * valid. */
    memset(config.data_dir, 'x', sizeof config.data_dir - 1u);
    config.data_dir[sizeof config.data_dir - 1u] = '\0';
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_OK);

    /* Restore a good directory and check the remaining fields. */
    ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, "data"), ASTRA_OK);

    config.log_level = (astra_log_level)77;
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_ERR_INVALID_STATE);
    config.log_level = ASTRA_LOG_INFO;

    config.page_size = 0u;
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_ERR_UNSUPPORTED);

    config.page_size = 3000u;
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_ERR_UNSUPPORTED);

    config.page_size = UINT32_MAX;
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_ERR_UNSUPPORTED);

    config.page_size = ASTRA_PAGE_SIZE_DEFAULT;
    ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_OK);

    /* Every accepted log level must pass validation, including NONE. */
    for (int level = ASTRA_LOG_TRACE; level < ASTRA_LOG_LEVEL_COUNT; ++level) {
        config.log_level = (astra_log_level)level;
        ASTRA_CHECK_STATUS(astra_config_validate(&config), ASTRA_OK);
    }
}

void astra_test_config_copy_and_describe(void)
{
    astra_config source;
    astra_config destination;
    char buffer[256];
    char narrow[16];
    int needed;

    astra_test_begin("astra_test_config_copy_and_describe");

    ASTRA_CHECK_STATUS(astra_config_init(&source), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_config_set_data_dir(&source, "/srv/astra"), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_config_set_log_level(&source, ASTRA_LOG_DEBUG), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_config_set_page_size(&source, 16384u), ASTRA_OK);

    /* A copy is fully independent of its source. */
    ASTRA_CHECK_STATUS(astra_config_copy(&destination, &source), ASTRA_OK);
    ASTRA_CHECK_STRING(destination.data_dir, "/srv/astra");
    ASTRA_CHECK(destination.log_level == ASTRA_LOG_DEBUG);
    ASTRA_CHECK(destination.page_size == 16384u);

    ASTRA_CHECK_STATUS(astra_config_set_data_dir(&source, "/elsewhere"), ASTRA_OK);
    ASTRA_CHECK_STRING(destination.data_dir, "/srv/astra");

    /* A self copy must be safe. */
    ASTRA_CHECK_STATUS(astra_config_copy(&destination, &destination), ASTRA_OK);
    ASTRA_CHECK_STRING(destination.data_dir, "/srv/astra");

    ASTRA_CHECK_STATUS(astra_config_copy(NULL, &source), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_config_copy(&destination, NULL), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_config_copy(NULL, NULL), ASTRA_ERR_INVALID_ARGUMENT);

    /* Rendering. */
    needed = astra_config_describe(&source, buffer, sizeof buffer);
    ASTRA_CHECK(needed > 0);
    ASTRA_CHECK((size_t)needed < sizeof buffer);
    ASTRA_CHECK(strstr(buffer, "data_dir=/elsewhere") != NULL);
    ASTRA_CHECK(strstr(buffer, "log_level=DEBUG") != NULL);
    ASTRA_CHECK(strstr(buffer, "page_size=16384") != NULL);
    ASTRA_CHECK(strchr(buffer, '\n') == NULL);

    /* Truncation follows snprintf semantics and stays NUL terminated. */
    needed = astra_config_describe(&source, narrow, sizeof narrow);
    ASTRA_CHECK(needed > (int)sizeof narrow);
    ASTRA_CHECK(strlen(narrow) == sizeof narrow - 1u);

    ASTRA_CHECK(astra_config_describe(&source, NULL, 0) == 0);
    ASTRA_CHECK(astra_config_describe(&source, buffer, 0) == 0);

    /* An absent configuration renders rather than crashing. */
    needed = astra_config_describe(NULL, buffer, sizeof buffer);
    ASTRA_CHECK(needed > 0);
    ASTRA_CHECK_STRING(buffer, "config: unset");

    /* The default level the configuration names is the one the logger starts
     * at, so applying a default configuration changes nothing. */
    ASTRA_CHECK(ASTRA_CONFIG_DEFAULT_LOG_LEVEL == ASTRA_LOG_INFO);
}