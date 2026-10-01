/*
 * Shared temporary database scaffolding for the storage tests.
 *
 * Both storage test files - the Disk Manager's and the Buffer Pool's - need a private,
 * empty directory per group, a configuration pointing at it, and a way to take it all
 * away again. That is the only code they have in common, and duplicating it would mean
 * two copies of the cleanup that a leaked directory would eventually expose as a test
 * that fails on a full disk rather than on a buffer pool bug.
 *
 * The directories are created under the process working directory, which is where CTest
 * puts it, and are named with the process id and a per-process counter. The counter
 * matters: without it, two groups running in the same process that both asked for
 * "astra_disk_test_1234" would collide, and which of them failed would depend on which
 * one cleaned up first.
 */
#ifndef ASTRA_TESTS_TEST_TEMP_H
#define ASTRA_TESTS_TEST_TEMP_H

#include "astra/astra.h"

#include <stddef.h>

/*
 * Directory creation, named rather than spelled out at each call site because the two
 * platforms disagree on both the function and the argument list.
 */
#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#  include <process.h>
#  define astra_test_mkdir(path) _mkdir(path)
#  define astra_test_rmdir(path) _rmdir(path)
#  define astra_test_getpid() ((long)_getpid())
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define astra_test_mkdir(path) mkdir((path), S_IRWXU)
#  define astra_test_rmdir(path) rmdir(path)
#  define astra_test_getpid() ((long)getpid())
#endif

/**
 * Creates an empty directory for one group's database and writes its path into `out`.
 *
 * Returns false only when the directory could not be made or the path would not fit,
 * which the caller turns into a failed check rather than an early return. A test that
 * cannot set up its fixture has failed, and returning early would hide that behind a
 * group that reported nothing.
 */
bool astra_test_make_temp_dir(char *out, size_t out_size);

/** Removes a directory made by astra_test_make_temp_dir, along with the file in it. */
void astra_test_remove_temp_dir(const char *dir);

/**
 * Fills `cfg` with the settings one temporary database is opened with.
 *
 * The page size is a parameter rather than a constant because the Disk Manager's page
 * size group opens databases at every accepted size, and a helper that hard-coded one
 * would make that group build its own.
 */
astra_status astra_test_make_config(astra_config *cfg, const char *dir, uint32 page_size);

/**
 * Writes the path of the primary data file inside `dir` into `out`.
 *
 * Needs the library's private path-join because the answer has to be exactly the path the
 * Disk Manager opens; a test that guessed the separator would pass on Windows and fail on
 * anything else, which is the kind of failure that only shows up in CI.
 */
bool astra_test_primary_path(char *out, size_t out_size, const char *dir);

/** Deletes the primary data file but keeps the directory. */
void astra_test_remove_primary_file(const char *dir);

#endif /* ASTRA_TESTS_TEST_TEMP_H */
