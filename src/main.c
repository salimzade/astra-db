#include "astra/astra.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    astra_config config;
    char summary[256];
    astra_status status;

    /*
     * Build the process configuration from the compiled-in defaults. Reading it
     * from a file or the command line is a later phase; the struct and its
     * defaults are real now so that the storage layer has something to be given.
     */
    status = astra_config_init(&config);
    if (status != ASTRA_OK) {
        fprintf(stderr, "%s: cannot build the default configuration\n", ASTRA_DB_NAME);
        return EXIT_FAILURE;
    }

    if (astra_init() != ASTRA_OK) {
        fprintf(stderr, "%s: initialization failed\n", ASTRA_DB_NAME);
        return EXIT_FAILURE;
    }

    (void)astra_log_set_level(config.log_level);

    if (astra_config_describe(&config, summary, sizeof summary) < 0) {
        fprintf(stderr, "%s: cannot render the configuration\n", ASTRA_DB_NAME);
        return EXIT_FAILURE;
    }

    printf("%s %s\n", ASTRA_DB_NAME, astra_version());
    printf("configuration: %s\n", summary);
    printf("Disk Manager and Buffer Pool available; no SQL, query engine or server yet.\n");

    /*
     * Flush stdout before logging. The log goes to stderr, which is unbuffered,
     * while stdout is block buffered whenever it is not a terminal, so without
     * this the log line appears first whenever the output is piped or
     * redirected, and last on a terminal. Flushing makes the order the same
     * everywhere, which matters for anything that reads this output.
     */
    (void)fflush(stdout);

    ASTRA_LOG_INFO("main", "%s %s ready", ASTRA_DB_NAME, astra_version());

    if (astra_shutdown() != ASTRA_OK) {
        fprintf(stderr, "%s: shutdown failed\n", ASTRA_DB_NAME);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}