/*
 * AstraDB storage: helpers shared by the storage translation units.
 *
 * Private. Not installed, not part of the public API, reachable only from inside
 * src/storage/ and from the unit tests, which link the static library directly.
 * The public storage contract is include/astra/storage/.
 */
#ifndef ASTRA_STORAGE_STORAGE_INTERNAL_H
#define ASTRA_STORAGE_STORAGE_INTERNAL_H

#include "astra/core/config.h"
#include "astra/core/types.h"
#include "astra/storage/format.h"

#include <stddef.h>

/*
 * Subsystem names reported in an astra_error and in log records.
 *
 * Each storage module reports under its own name, so a log line or an error says
 * which layer refused. A single "storage" name for all three would leave a reader
 * guessing which of the three had the bad argument.
 */
#define ASTRA_SUBSYSTEM_DISK "disk"
#define ASTRA_SUBSYSTEM_FILE "file"
#define ASTRA_SUBSYSTEM_PAGE "page"

/*
 * ---------------------------------------------------------------------------
 * Byte order
 * ---------------------------------------------------------------------------
 *
 * The on-disk format is little-endian and is written byte by byte rather than by
 * copying a host integer into place. Three reasons, in order of importance:
 *
 *   1. It is the only way the file reads identically on a big-endian host. The
 *      project targets Linux and Windows, both of which are little-endian today,
 *      but a format that depends on that is a format that is wrong somewhere.
 *   2. It avoids every alignment and strict-aliasing question that copying
 *      through a uint32_t pointer raises, and with it the `-Wcast-align` and
 *      `-Wcast-qual` warnings that exist precisely to catch such code.
 *   3. It makes the byte table in format.h and the code that produces it readable
 *      side by side.
 *
 * These are non-static so that the tests can round-trip them against known
 * vectors. They are internal, not public: a caller that needs to decode the
 * header has a use for a purpose-built decoder, not for these primitives.
 */

/** Stores `value` as 4 little-endian bytes at `dst`. `dst` must have 4 bytes. */
void astra_store_u32_le(uint8 *dst, uint32 value);

/** Stores `value` as 8 little-endian bytes at `dst`. `dst` must have 8 bytes. */
void astra_store_u64_le(uint8 *dst, uint64 value);

/** Loads 4 little-endian bytes from `src`. `src` must have 4 bytes. */
uint32 astra_load_u32_le(const uint8 *src);

/** Loads 8 little-endian bytes from `src`. `src` must have 8 bytes. */
uint64 astra_load_u64_le(const uint8 *src);

/**
 * Computes the CRC-32 of `data`, as specified in format.h.
 *
 * Parameters:
 *   data   - bytes to checksum. NULL is allowed only when `length` is 0.
 *   length - number of bytes to read from `data`.
 *
 * Returns: the checksum. Never fails, never allocates, and has no state: the
 * implementation is a bitwise CRC with no lookup table, so there is no mutable
 * global to guard and nothing to initialise.
 */
uint32 astra_checksum32(const uint8 *data, size_t length);

/*
 * ---------------------------------------------------------------------------
 * Paths
 * ---------------------------------------------------------------------------
 */

/** Capacity of the buffer needed to hold any joined database path. */
#define ASTRA_STORAGE_PATH_MAX (ASTRA_CONFIG_PATH_MAX + 64u)

/**
 * Joins a directory and a file name with a single separator into `out`.
 *
 * A trailing separator on `dir` is not doubled. `out` is written only on success.
 *
 * Returns ASTRA_OK, or ASTRA_ERR_OUT_OF_MEMORY if the result does not fit,
 * which is reported as out of memory for the same reason astra_config_set_data_dir
 * reports it: the path is valid, it is the buffer that is too small.
 */
astra_status astra_path_join(char *out,
                             size_t out_size,
                             const char *dir,
                             const char *name);

/**
 * Creates a single directory, tolerating one that already exists.
 *
 * Only the final path component is created; `path`'s parent must already exist.
 * See astra_disk_manager_create for why. Never creates anything above the leaf.
 */
astra_status astra_directory_create(const char *path);

#endif /* ASTRA_STORAGE_STORAGE_INTERNAL_H */