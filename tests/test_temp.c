#include "test_temp.h"

#include "storage/database_file.h"
#include "storage/storage_internal.h"

#include <stdio.h>

/*
 * Distinguishes groups that would otherwise pick the same name. Not thread safe, and
 * does not need to be: the fixture is created on the main thread before a group's worker
 * threads exist.
 */
static unsigned int g_temp_counter;

bool astra_test_make_temp_dir(char *out, size_t out_size)
{
    int written;

    written = snprintf(out, out_size, "astra_disk_test_%ld_%u",
                       astra_test_getpid(), ++g_temp_counter);
    if (written < 0 || (size_t)written >= out_size) {
        return false;
    }

    if (astra_test_mkdir(out) != 0) {
        return false;
    }
    return true;
}

void astra_test_remove_temp_dir(const char *dir)
{
    char path[ASTRA_STORAGE_PATH_MAX];

    if (astra_path_join(path, sizeof path, dir, ASTRA_DISK_MANAGER_PRIMARY_FILE)
        == ASTRA_OK) {
        (void)remove(path);
    }
    (void)astra_test_rmdir(dir);
}

astra_status astra_test_make_config(astra_config *cfg, const char *dir, uint32 page_size)
{
    astra_status status;

    status = astra_config_init(cfg);
    if (status != ASTRA_OK) {
        return status;
    }

    status = astra_config_set_data_dir(cfg, dir);
    if (status != ASTRA_OK) {
        return status;
    }

    return astra_config_set_page_size(cfg, page_size);
}

bool astra_test_primary_path(char *out, size_t out_size, const char *dir)
{
    return astra_path_join(out, out_size, dir, ASTRA_DISK_MANAGER_PRIMARY_FILE) == ASTRA_OK;
}

void astra_test_remove_primary_file(const char *dir)
{
    char path[ASTRA_STORAGE_PATH_MAX];

    if (astra_path_join(path, sizeof path, dir, ASTRA_DISK_MANAGER_PRIMARY_FILE)
        == ASTRA_OK) {
        (void)remove(path);
    }
}
