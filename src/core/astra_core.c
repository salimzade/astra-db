#include "astra/astra.h"

#include <stdbool.h>
#include <stdatomic.h>

/*
 * Process wide library state.
 *
 * This is the only mutable global in the codebase. It is owned exclusively by
 * this translation unit, is never exposed, and is accessed through C11 atomics
 * so that concurrent astra_init()/astra_shutdown() calls stay well defined.
 */
static atomic_bool g_initialized;

const char *astra_version(void)
{
    return ASTRA_DB_VERSION_STRING;
}

astra_status astra_init(void)
{
    bool expected = false;

    if (atomic_compare_exchange_strong(&g_initialized, &expected, true)) {
        return ASTRA_OK;
    }
    return ASTRA_ERR_ALREADY_INITIALIZED;
}

astra_status astra_shutdown(void)
{
    bool expected = true;

    if (atomic_compare_exchange_strong(&g_initialized, &expected, false)) {
        return ASTRA_OK;
    }
    return ASTRA_ERR_NOT_INITIALIZED;
}
