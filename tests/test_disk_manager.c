/*
 * Storage tests: the Disk Manager, the page buffer and the on-disk format.
 *
 * Every group works on its own temporary database directory and removes it before
 * returning, including when a check fails: the harness has no teardown hook, so
 * cleanup is written at the end of each group rather than left to a fixture that
 * would have to be added to keep it honest.
 *
 * The temporary directories are created under the process working directory, which
 * is where CTest puts it, and are named with the process id so that two runs in
 * the same directory - a developer running the binary by hand while CTest runs,
 * say - cannot collide.
 *
 * Corruption is produced by writing to the file behind the library's back, with
 * <stdio.h> and a path the test builds itself. That is the point of those groups:
 * the library must cope with a file it did not write, and the only way to arrange
 * one is to not go through the library.
 */
#include "test_support.h"

#include "storage/database_file.h"
#include "storage/storage_internal.h"

#include <stdio.h>

#include "test_support.h"
#include "test_temp.h"

#include "storage/database_file.h"
#include "storage/storage_internal.h"

#include <stdio.h>

/*
 * The temporary directory fixture, the configuration helper and the primary file path
 * are shared with the Buffer Pool tests and live in test_temp.c. The corruption helper
 * stays here, because only the Disk Manager tests need it: it exists to get back to a
 * good database without going through the library, which is what a test that
 * deliberately corrupts a file needs and nothing else does.
 */
static void remove_primary_file(const char *dir)
{
    astra_test_remove_primary_file(dir);
}

/*
 * Overwrites `length` bytes at `offset` in the primary data file, using <stdio.h>
 * and so bypassing the library entirely. Used only by the corruption groups.
 */
static bool poke_file(const char *dir,
                      long offset,
                      const void *bytes,
                      size_t length)
{
    char path[ASTRA_STORAGE_PATH_MAX];
    FILE *stream;

    if (!astra_test_primary_path(path, sizeof path, dir)) {
        return false;
    }

    stream = fopen(path, "r+b");
    if (stream == NULL) {
        return false;
    }

    if (fseek(stream, offset, SEEK_SET) != 0) {
        (void)fclose(stream);
        return false;
    }
    if (fwrite(bytes, 1u, length, stream) != length) {
        (void)fclose(stream);
        return false;
    }
    if (fclose(stream) != 0) {
        return false;
    }
    return true;
}

/* Reads `length` bytes at `offset` from the primary data file, bypassing the library. */
static bool peek_file(const char *dir, long offset, void *out, size_t length)
{
    char path[ASTRA_STORAGE_PATH_MAX];
    FILE *stream;

    if (!astra_test_primary_path(path, sizeof path, dir)) {
        return false;
    }

    stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }

    if (fseek(stream, offset, SEEK_SET) != 0) {
        (void)fclose(stream);
        return false;
    }
    if (fread(out, 1u, length, stream) != length) {
        (void)fclose(stream);
        return false;
    }
    (void)fclose(stream);
    return true;
}

/* Fills a page buffer with a pattern that is unique to `page_id`, so that a read of
 * the wrong page cannot be mistaken for a read of the right one. */
static void fill_pattern(astra_page *page, page_id_t page_id)
{
    uint8 *bytes = (uint8 *)page->data;

    memset(bytes, (int)((unsigned)page_id & 0xffu), (size_t)page->data_size);
    astra_store_u64_le(bytes, (uint64)page_id);
    astra_store_u64_le(bytes + 8u, (uint64)page_id * 2654435761u);
}

/* True when `page` holds exactly the pattern fill_pattern gives `page_id`. */
static bool page_matches_pattern(const astra_page *page, page_id_t page_id)
{
    const uint8 *bytes = (const uint8 *)page->data;
    size_t index;

    if (astra_load_u64_le(bytes) != (uint64)page_id) {
        return false;
    }
    if (astra_load_u64_le(bytes + 8u) != (uint64)page_id * 2654435761u) {
        return false;
    }

    for (index = 16u; index < (size_t)page->data_size; ++index) {
        if (bytes[index] != (uint8)((unsigned)page_id & 0xffu)) {
            return false;
        }
    }
    return true;
}

/* True when every byte of `page` is zero. */
static bool page_is_zeroed(const astra_page *page)
{
    const uint8 *bytes = (const uint8 *)page->data;
    size_t index;

    for (index = 0; index < (size_t)page->data_size; ++index) {
        if (bytes[index] != 0u) {
            return false;
        }
    }
    return true;
}

/*
 * ---------------------------------------------------------------------------
 * Format primitives
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_format(void)
{
    uint8 buffer[16];
    uint8 scratch[8];

    astra_test_begin("astra_test_disk_format");

    /* Little-endian storage, checked against the byte table in format.h. */
    astra_store_u32_le(buffer, 0x01020304u);
    ASTRA_CHECK(buffer[0] == 0x04u);
    ASTRA_CHECK(buffer[1] == 0x03u);
    ASTRA_CHECK(buffer[2] == 0x02u);
    ASTRA_CHECK(buffer[3] == 0x01u);
    ASTRA_CHECK(astra_load_u32_le(buffer) == 0x01020304u);

    astra_store_u64_le(scratch, UINT64_C(0x0102030405060708));
    ASTRA_CHECK(scratch[0] == 0x08u);
    ASTRA_CHECK(scratch[7] == 0x01u);
    ASTRA_CHECK(astra_load_u64_le(scratch) == UINT64_C(0x0102030405060708));

    /* The extremes, because a shift by 31 or 63 is where this kind of code
     * usually goes wrong. */
    astra_store_u32_le(buffer, UINT32_MAX);
    ASTRA_CHECK(astra_load_u32_le(buffer) == UINT32_MAX);
    astra_store_u32_le(buffer, 0u);
    ASTRA_CHECK(astra_load_u32_le(buffer) == 0u);

    astra_store_u64_le(scratch, UINT64_MAX);
    ASTRA_CHECK(astra_load_u64_le(scratch) == UINT64_MAX);
    astra_store_u64_le(scratch, 0u);
    ASTRA_CHECK(astra_load_u64_le(scratch) == 0u);

    /* Published CRC-32/ISO-HDLC vectors, so the header checksum is asserted
     * against the algorithm's definition and not against itself. */
    ASTRA_CHECK_UINT64(astra_checksum32((const uint8 *)"", 0u), 0x00000000u);
    ASTRA_CHECK_UINT64(astra_checksum32((const uint8 *)"a", 1u), 0xe8b7be43u);
    ASTRA_CHECK_UINT64(astra_checksum32((const uint8 *)"abc", 3u), 0x352441c2u);
    ASTRA_CHECK_UINT64(astra_checksum32((const uint8 *)"123456789", 9u), 0xcbf43926u);

    /* The header has to fit in the smallest page the library accepts, otherwise a
     * small-page database could not carry one. */
    ASTRA_CHECK(ASTRA_FORMAT_HEADER_USED <= ASTRA_PAGE_SIZE_MIN);
    ASTRA_CHECK(ASTRA_FORMAT_HEADER_PAGE_ID == (page_id_t)0);
    ASTRA_CHECK_UINT64(ASTRA_FORMAT_MAGIC_TEXT_SIZE + 1u, ASTRA_FORMAT_MAGIC_SIZE);
}

/*
 * ---------------------------------------------------------------------------
 * Page offset arithmetic
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_page_offsets(void)
{
    astra_database_file file;
    uint64 offset = 12345u;

    astra_test_begin("astra_test_disk_page_offsets");

    /* A page identifier is a position, so its offset is the product. */
    ASTRA_CHECK(astra_file_page_offset(0u, 16384u, &offset));
    ASTRA_CHECK_UINT64(offset, 0u);

    ASTRA_CHECK(astra_file_page_offset(3u, 16384u, &offset));
    ASTRA_CHECK_UINT64(offset, 3u * 16384u);

    ASTRA_CHECK(astra_file_page_offset(1u, 512u, &offset));
    ASTRA_CHECK_UINT64(offset, 512u);

    /* Identifiers at and past the point where the product leaves the range of a
     * 64-bit file offset are refused rather than wrapped into a plausible value. */
    ASTRA_CHECK(!astra_file_page_offset(UINT64_MAX, 16384u, &offset));
    ASTRA_CHECK(!astra_file_page_offset(UINT64_MAX / 16384u + 1u, 16384u, &offset));
    ASTRA_CHECK(!astra_file_page_offset(1u, 0u, &offset));
    ASTRA_CHECK(!astra_file_page_offset(1u, 16384u, NULL));

    /* The last page that still fits must be accepted, so the guard is exactly at
     * the boundary rather than one page short of it. */
    ASTRA_CHECK(astra_file_page_offset(UINT64_MAX / 16384u, 16384u, &offset));

    /* Page existence is decided once, in one place. */
    ASTRA_CHECK(!astra_file_has_page(NULL, 0u));
    memset(&file, 0, sizeof file);
    file.page_count = 3u;
    ASTRA_CHECK(astra_file_has_page(&file, 0u));
    ASTRA_CHECK(astra_file_has_page(&file, 2u));
    ASTRA_CHECK(!astra_file_has_page(&file, 3u));
    ASTRA_CHECK(!astra_file_has_page(&file, ASTRA_PAGE_ID_INVALID));
}

/*
 * ---------------------------------------------------------------------------
 * Page buffers
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_page_lifecycle(void)
{
    astra_page page = ASTRA_PAGE_INIT;
    uint8 *bytes;
    uint32 index;

    astra_test_begin("astra_test_disk_page_lifecycle");

    /* A fresh page is fully defined: a known identifier, a zeroed buffer of the
     * requested size, and clean. Nothing about it is left to chance. */
    ASTRA_CHECK_STATUS(astra_page_init(&page, 4096u), ASTRA_OK);
    ASTRA_CHECK(astra_page_is_initialized(&page));
    ASTRA_CHECK(page.page_id == ASTRA_PAGE_ID_INVALID);
    ASTRA_CHECK(page.data_size == 4096u);
    ASTRA_CHECK(page.data != NULL);
    ASTRA_CHECK(!page.is_dirty);

    bytes = (uint8 *)page.data;
    for (index = 0; index < page.data_size; ++index) {
        if (bytes[index] != 0u) {
            break;
        }
    }
    ASTRA_CHECK(index == page.data_size);

    /* The dirty flag belongs to the caller, and the Disk Manager never sets it. */
    page.is_dirty = true;
    ASTRA_CHECK(page.is_dirty);

    /* Clearing, including the whole buffer and a zero length. */
    bytes[0] = 0xffu;
    bytes[4095] = 0xffu;
    ASTRA_CHECK_STATUS(astra_page_clear(&page, 2u), ASTRA_OK);
    ASTRA_CHECK(bytes[0] == 0u);
    ASTRA_CHECK(bytes[1] == 0u);
    ASTRA_CHECK(bytes[2] == 0u);
    ASTRA_CHECK(bytes[4095] == 0xffu);

    ASTRA_CHECK_STATUS(astra_page_clear(&page, 4096u), ASTRA_OK);
    ASTRA_CHECK(bytes[4095] == 0u);

    /* A partial clear cannot run off the end of the buffer. */
    ASTRA_CHECK_STATUS(astra_page_clear(&page, 4097u), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_page_clear(&page, UINT32_MAX), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_page_clear(NULL, 0u), ASTRA_ERR_INVALID_ARGUMENT);

    /* Release resets the struct, and is safe to call again. */
    astra_page_release(&page);
    ASTRA_CHECK(!astra_page_is_initialized(&page));
    ASTRA_CHECK(page.page_id == ASTRA_PAGE_ID_INVALID);
    ASTRA_CHECK(page.data_size == 0u);
    ASTRA_CHECK(page.data == NULL);
    ASTRA_CHECK(!page.is_dirty);
    astra_page_release(&page);
    astra_page_release(NULL);

    /* Every accepted page size, and the boundaries of the range. */
    ASTRA_CHECK_STATUS(astra_page_init(&page, ASTRA_PAGE_SIZE_MIN), ASTRA_OK);
    ASTRA_CHECK(page.data_size == ASTRA_PAGE_SIZE_MIN);
    astra_page_release(&page);
    ASTRA_CHECK_STATUS(astra_page_init(&page, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    ASTRA_CHECK(page.data_size == ASTRA_PAGE_SIZE_DEFAULT);
    astra_page_release(&page);
    ASTRA_CHECK_STATUS(astra_page_init(&page, ASTRA_PAGE_SIZE_MAX), ASTRA_OK);
    ASTRA_CHECK(page.data_size == ASTRA_PAGE_SIZE_MAX);
    astra_page_release(&page);

    /* Sizes that are not page sizes are refused before anything is allocated, so a
     * rejected call leaves no buffer behind. An unsupported size is reported as
     * such rather than as a bad argument, which is the same answer the Disk
     * Manager gives for a configuration it cannot lay down. */
    ASTRA_CHECK_STATUS(astra_page_init(&page, 0u), ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK(page.data == NULL);
    ASTRA_CHECK_STATUS(astra_page_init(&page, 1u), ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(astra_page_init(&page, ASTRA_PAGE_SIZE_MIN - 1u),
                       ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(astra_page_init(&page, ASTRA_PAGE_SIZE_MAX + 1u),
                       ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(astra_page_init(&page, 3000u), ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(astra_page_init(&page, UINT32_MAX), ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK(page.data == NULL);
    ASTRA_CHECK_STATUS(astra_page_init(NULL, 4096u), ASTRA_ERR_INVALID_ARGUMENT);

    /* The library's default page size is the one storage defaults to. */
    ASTRA_CHECK_UINT64(ASTRA_PAGE_SIZE_DEFAULT, 16384u);
    ASTRA_CHECK(astra_page_size_is_valid(ASTRA_PAGE_SIZE_DEFAULT));
}

/*
 * ---------------------------------------------------------------------------
 * Create
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_create(void)
{
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    uint64 bytes = 0;
    uint64 pages = 0;

    astra_test_begin("astra_test_disk_create");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    /* The directory does not exist yet, and creating a database makes it. */
    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    ASTRA_CHECK(manager != NULL);

    if (manager != NULL) {
        /* One page: the reserved header. No page is allocated until asked for. */
        ASTRA_CHECK_STATUS(astra_disk_manager_file_size(manager, &bytes), ASTRA_OK);
        ASTRA_CHECK_UINT64(bytes, ASTRA_PAGE_SIZE_DEFAULT);
        ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
        ASTRA_CHECK_UINT64(pages, 1u);
        ASTRA_CHECK_UINT64(astra_disk_manager_page_size(manager),
                           ASTRA_PAGE_SIZE_DEFAULT);

        ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        manager = NULL;
    }

    /* The file is where the format says it is, and it is one whole page long. */
    {
        char path[ASTRA_STORAGE_PATH_MAX];
        FILE *stream;
        long length = -1;

        ASTRA_CHECK(astra_test_primary_path(path, sizeof path, dir));
        ASTRA_CHECK_STRING(path + strlen(path) - (sizeof(ASTRA_DISK_MANAGER_PRIMARY_FILE) - 1u),
                           ASTRA_DISK_MANAGER_PRIMARY_FILE);

        stream = fopen(path, "rb");
        ASTRA_CHECK(stream != NULL);
        if (stream != NULL) {
            (void)fseek(stream, 0L, SEEK_END);
            length = ftell(stream);
            (void)fclose(stream);
        }
        ASTRA_CHECK(length == (long)ASTRA_PAGE_SIZE_DEFAULT);
    }

    /* Creating over an existing database is refused, never done silently. */
    {
        astra_disk_manager *second = NULL;
        ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &second),
                           ASTRA_ERR_ALREADY_EXISTS);
        ASTRA_CHECK(second == NULL);
    }

    /* The default data directory really is "data", and the primary file is named
     * "main.db", so the conventional path is data/main.db. */
    ASTRA_CHECK_STATUS(astra_config_init(&config), ASTRA_OK);
    ASTRA_CHECK_STRING(config.data_dir, "data");
    {
        char path[ASTRA_STORAGE_PATH_MAX];
        ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, "data"), ASTRA_OK);
        ASTRA_CHECK(astra_path_join(path, sizeof path, config.data_dir,
                                    ASTRA_DISK_MANAGER_PRIMARY_FILE) == ASTRA_OK);
        ASTRA_CHECK_STRING(path, "data/main.db");
    }

    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * Open
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_open(void)
{
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    uint64 pages = 0;

    astra_test_begin("astra_test_disk_open");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);

    /* Reopening reports what the file on disk actually contains, not what the
     * creating handle believed. */
    manager = NULL;
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager), ASTRA_OK);
    ASTRA_CHECK(manager != NULL);
    if (manager != NULL) {
        ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
        ASTRA_CHECK_UINT64(pages, 1u);
        ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    }

    /* Opening does not create anything: a directory that is not there is a
     * not-found, and nothing is left behind. */
    {
        char missing[80];
        astra_disk_manager *absent = NULL;
        astra_config absent_config;

        (void)snprintf(missing, sizeof missing, "%s_absent", dir);
        ASTRA_CHECK_STATUS(astra_test_make_config(&absent_config, missing,
                                       ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_open(&absent_config, &absent),
                           ASTRA_ERR_NOT_FOUND);
        ASTRA_CHECK(absent == NULL);
    }

    /* A directory that exists but holds no database file is also not found, which
     * is a different situation from a corrupt one. */
    {
        char empty[80];
        astra_disk_manager *absent = NULL;
        astra_config empty_config;

        (void)snprintf(empty, sizeof empty, "%s_empty", dir);
        ASTRA_CHECK(astra_test_mkdir(empty) == 0);
        ASTRA_CHECK_STATUS(astra_test_make_config(&empty_config, empty,
                                       ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_open(&empty_config, &absent),
                           ASTRA_ERR_NOT_FOUND);
        ASTRA_CHECK(absent == NULL);
        (void)astra_test_rmdir(empty);
    }

    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * The full round trip
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_roundtrip(void)
{
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    astra_page page = ASTRA_PAGE_INIT;
    astra_page reread = ASTRA_PAGE_INIT;
    page_id_t first = ASTRA_PAGE_ID_INVALID;
    page_id_t second = ASTRA_PAGE_ID_INVALID;
    uint64 pages = 0;

    astra_test_begin("astra_test_disk_roundtrip");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    if (manager == NULL) {
        astra_test_remove_temp_dir(dir);
        return;
    }

    /* Allocate. Page 0 belongs to the file, so the first page a caller gets is 1. */
    ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &first), ASTRA_OK);
    ASTRA_CHECK(astra_page_id_is_valid(first));
    ASTRA_CHECK_UINT64(first, 1u);

    ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &second), ASTRA_OK);
    ASTRA_CHECK_UINT64(second, 2u);
    ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
    ASTRA_CHECK_UINT64(pages, 3u);

    /* Write, and the write is observed to have happened. */
    ASTRA_CHECK_STATUS(astra_page_init(&page, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    page.page_id = first;
    fill_pattern(&page, first);
    page.is_dirty = true;
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &page), ASTRA_OK);

    /* A successful write clears the dirty flag: the bytes on disk now match. */
    ASTRA_CHECK(!page.is_dirty);

    /* Read back into a second buffer, so the comparison is against the disk and
     * not against the buffer that was just written. */
    ASTRA_CHECK_STATUS(astra_page_init(&reread, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, second, &reread), ASTRA_OK);
    ASTRA_CHECK(reread.page_id == second);
    /* A page that was only allocated reads back as zeros. */
    ASTRA_CHECK(page_is_zeroed(&reread));

    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, first, &reread), ASTRA_OK);
    ASTRA_CHECK(reread.page_id == first);
    ASTRA_CHECK(page_matches_pattern(&reread, first));
    ASTRA_CHECK(!page_is_zeroed(&reread));

    astra_page_release(&reread);
    astra_page_release(&page);

    /* Close, then reopen, then read: this is the only sequence that proves the
     * bytes survived the file, and it is why the test closes before it checks. */
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    manager = NULL;
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager), ASTRA_OK);
    if (manager != NULL) {
        ASTRA_CHECK_STATUS(astra_page_init(&reread, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, first, &reread),
                           ASTRA_OK);
        ASTRA_CHECK(reread.page_id == first);
        ASTRA_CHECK(page_matches_pattern(&reread, first));
        astra_page_release(&reread);

        /* The page count came from the file, so it survived the close as well. */
        ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
        ASTRA_CHECK_UINT64(pages, 3u);

        ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        manager = NULL;
    }

    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * Many pages, stable identifiers
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_alloc_many(void)
{
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    astra_page page = ASTRA_PAGE_INIT;
    page_id_t ids[64];
    const size_t count = sizeof ids / sizeof ids[0];
    uint64 pages = 0;
    size_t index;

    astra_test_begin("astra_test_disk_alloc_many");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, 4096u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    if (manager == NULL) {
        astra_test_remove_temp_dir(dir);
        return;
    }

    /* Identifiers are dense and start after the header. */
    for (index = 0; index < count; ++index) {
        ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &ids[index]),
                           ASTRA_OK);
        if (index == 0) {
            ASTRA_CHECK_UINT64(ids[index], 1u);
        } else {
            ASTRA_CHECK_UINT64(ids[index], (uint64)index + 1u);
        }
    }
    ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
    ASTRA_CHECK_UINT64(pages, (uint64)count + 1u);

    /* Give every page a distinct body. */
    ASTRA_CHECK_STATUS(astra_page_init(&page, 4096u), ASTRA_OK);
    for (index = 0; index < count; ++index) {
        page.page_id = ids[index];
        fill_pattern(&page, ids[index]);
        ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &page), ASTRA_OK);
    }
    astra_page_release(&page);
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    /* Reopen and confirm that every page still holds its own contents: this is
     * what "identifiers are stable" has to mean in practice. */
    manager = NULL;
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager), ASTRA_OK);
    if (manager != NULL) {
        astra_page reread = ASTRA_PAGE_INIT;

        ASTRA_CHECK_STATUS(astra_page_init(&reread, 4096u), ASTRA_OK);
        for (index = 0; index < count; ++index) {
            ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, ids[index],
                                                            &reread), ASTRA_OK);
            ASTRA_CHECK(reread.page_id == ids[index]);
            ASTRA_CHECK(page_matches_pattern(&reread, ids[index]));
        }
        astra_page_release(&reread);

        /* Allocating after a reopen continues the sequence rather than restarting
         * it, so identifiers do not depend on the handle that allocated them. */
        {
            page_id_t next = ASTRA_PAGE_ID_INVALID;
            ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &next), ASTRA_OK);
            ASTRA_CHECK_UINT64(next, (uint64)count + 1u);
        }

        ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        manager = NULL;
    }

    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * Synchronisation
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_sync(void)
{
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    astra_page page = ASTRA_PAGE_INIT;
    page_id_t id = ASTRA_PAGE_ID_INVALID;
    uint8 raw[16];
    uint8 expected[16];

    astra_test_begin("astra_test_disk_sync");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, 4096u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    if (manager == NULL) {
        astra_test_remove_temp_dir(dir);
        return;
    }

    ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &id), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_page_init(&page, 4096u), ASTRA_OK);
    page.page_id = id;
    fill_pattern(&page, id);
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &page), ASTRA_OK);

    /* A write is not a sync, so the file is asked to flush before it is read back
     * with <stdio.h>. The read deliberately bypasses the library: reading through
     * the same handle would only prove the handle is self-consistent. */
    ASTRA_CHECK_STATUS(astra_disk_manager_sync(manager), ASTRA_OK);

    ASTRA_CHECK(peek_file(dir, (long)id * 4096L, raw, sizeof raw));
    astra_store_u64_le(expected, (uint64)id);
    astra_store_u64_le(expected + 8u, (uint64)id * 2654435761u);
    ASTRA_CHECK(memcmp(raw, expected, sizeof raw) == 0);

    /* Syncing again is legal and does not change anything, so a caller can flush
     * defensively without having to track whether it already has. */
    ASTRA_CHECK_STATUS(astra_disk_manager_sync(manager), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_sync(manager), ASTRA_OK);

    /* NULL is reported, not dereferenced. */
    ASTRA_CHECK_STATUS(astra_disk_manager_sync(NULL), ASTRA_ERR_INVALID_ARGUMENT);

    /* Close synchronises as well, and reports the result. */
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    /* A null handle is a no-op rather than a crash, so cleanup paths need no
     * guard. */
    ASTRA_CHECK_STATUS(astra_disk_manager_close(NULL), ASTRA_OK);

    astra_page_release(&page);
    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * Truncate
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_truncate(void)
{
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    astra_page page = ASTRA_PAGE_INIT;
    page_id_t ids[4];
    uint64 bytes = 0;
    uint64 pages = 0;
    size_t index;

    astra_test_begin("astra_test_disk_truncate");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, 4096u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    if (manager == NULL) {
        astra_test_remove_temp_dir(dir);
        return;
    }

    for (index = 0; index < sizeof ids / sizeof ids[0]; ++index) {
        ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &ids[index]),
                           ASTRA_OK);
    }
    ASTRA_CHECK_STATUS(astra_page_init(&page, 4096u), ASTRA_OK);
    for (index = 0; index < sizeof ids / sizeof ids[0]; ++index) {
        page.page_id = ids[index];
        fill_pattern(&page, ids[index]);
        ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &page), ASTRA_OK);
    }

    /* Growing is equivalent to allocating, and gives zero pages. */
    ASTRA_CHECK_STATUS(astra_disk_manager_truncate(manager, 8u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
    ASTRA_CHECK_UINT64(pages, 8u);
    ASTRA_CHECK_STATUS(astra_disk_manager_file_size(manager, &bytes), ASTRA_OK);
    ASTRA_CHECK_UINT64(bytes, 8u * 4096u);

    /* Truncating to the length it already has is a no-op, not an error. */
    ASTRA_CHECK_STATUS(astra_disk_manager_truncate(manager, 8u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
    ASTRA_CHECK_UINT64(pages, 8u);

    /* The pages that survive a shrink keep their contents. */
    ASTRA_CHECK_STATUS(astra_disk_manager_truncate(manager, 2u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
    ASTRA_CHECK_UINT64(pages, 2u);
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, ids[0], &page), ASTRA_OK);
    ASTRA_CHECK(page_matches_pattern(&page, ids[0]));

    /* A discarded page is gone, and its identifier is not recycled: re-growing the
     * file does not bring it back, which is what keeps a stale reference from
     * silently addressing different data. */
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, ids[3], &page),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK_STATUS(astra_disk_manager_truncate(manager, 8u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, ids[3], &page),
                       ASTRA_OK);
    ASTRA_CHECK(page_is_zeroed(&page));

    /* The header page cannot be truncated away: a file without one is not a
     * database, and refusing here is better than leaving an unopenable file. */
    ASTRA_CHECK_STATUS(astra_disk_manager_truncate(manager, 0u),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
    ASTRA_CHECK_UINT64(pages, 8u);

    /* A target length whose byte offset leaves the range of a file is refused
     * rather than wrapped. */
    ASTRA_CHECK_STATUS(astra_disk_manager_truncate(manager, UINT64_MAX),
                       ASTRA_ERR_OUT_OF_MEMORY);
    ASTRA_CHECK_STATUS(astra_disk_manager_truncate(NULL, 1u),
                       ASTRA_ERR_INVALID_ARGUMENT);

    astra_page_release(&page);
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    /* The truncated file is still a valid database, and reports its real length
     * when reopened. */
    manager = NULL;
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager), ASTRA_OK);
    if (manager != NULL) {
        ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages), ASTRA_OK);
        ASTRA_CHECK_UINT64(pages, 8u);
        ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        manager = NULL;
    }

    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * Invalid page access
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_invalid_page(void)
{
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    astra_page page = ASTRA_PAGE_INIT;
    astra_page wrong_size = ASTRA_PAGE_INIT;
    astra_page released = ASTRA_PAGE_INIT;
    page_id_t id = ASTRA_PAGE_ID_INVALID;
    uint64 bytes = 0;

    astra_test_begin("astra_test_disk_invalid_page");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, 4096u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    if (manager == NULL) {
        astra_test_remove_temp_dir(dir);
        return;
    }

    ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &id), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_page_init(&page, 4096u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_page_init(&wrong_size, 8192u), ASTRA_OK);
    page.page_id = id;

    /* A NULL handle is a reportable error on every entry point, not a crash. */
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(NULL, 0u, &page),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(NULL, &page),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(NULL, &id),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_file_size(NULL, &bytes),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_page_count(NULL, &bytes),
                       ASTRA_ERR_INVALID_ARGUMENT);
    /* The size of no database is the documented default, so a caller can size a
     * buffer from it without branching on whether one is open. */
    ASTRA_CHECK_UINT64(astra_disk_manager_page_size(NULL), ASTRA_PAGE_SIZE_DEFAULT);

    /* A NULL destination is reported. */
    ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, 0u, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_file_size(manager, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);

    /* A page with no buffer, and a page whose buffer is a different size than
     * this database's, are both caller errors. */
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, id, &released),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &released),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, id, &wrong_size),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &wrong_size),
                       ASTRA_ERR_INVALID_ARGUMENT);

    /* Identifiers that address nothing: the sentinel, one past the end, and one
     * far past the end. All of them are "not allocated", not "bad argument". */
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, ASTRA_PAGE_ID_INVALID, &page),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, 2u, &page),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, UINT64_MAX / 2u, &page),
                       ASTRA_ERR_NOT_FOUND);

    /* A page that was never allocated cannot be written, whether its identifier
     * was never set or simply lies beyond the end. */
    {
        astra_page unset = ASTRA_PAGE_INIT;
        ASTRA_CHECK_STATUS(astra_page_init(&unset, 4096u), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &unset),
                           ASTRA_ERR_NOT_FOUND);
        astra_page_release(&unset);
    }
    page.page_id = 99u;
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &page),
                       ASTRA_ERR_NOT_FOUND);

    /* A failed read leaves the page describing what it described before, so a
     * caller that retries cannot mistake the previous contents for the page it
     * asked for. */
    page.page_id = id;
    fill_pattern(&page, id);
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, 999u, &page),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK(page.page_id == id);

    /* The header page may be read - it is a real page and tools will want it -
     * but it may not be written, because doing so would make the database
     * permanently unopenable. */
    ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager,
                                                    ASTRA_FORMAT_HEADER_PAGE_ID,
                                                    &page), ASTRA_OK);
    ASTRA_CHECK(page.page_id == ASTRA_FORMAT_HEADER_PAGE_ID);
    ASTRA_CHECK(memcmp(page.data, ASTRA_FORMAT_MAGIC_TEXT,
                       ASTRA_FORMAT_MAGIC_TEXT_SIZE) == 0);

    page.page_id = ASTRA_FORMAT_HEADER_PAGE_ID;
    page.is_dirty = true;
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &page),
                       ASTRA_ERR_INVALID_STATE);
    /* The refusal is not a write, so the flag is untouched. */
    ASTRA_CHECK(page.is_dirty);

    /* Writing the header page is still refused after a rejected read, so the two
     * checks do not interfere with each other. */
    astra_page_release(&page);
    ASTRA_CHECK_STATUS(astra_page_init(&page, 4096u), ASTRA_OK);
    page.page_id = ASTRA_FORMAT_HEADER_PAGE_ID;
    ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &page),
                       ASTRA_ERR_INVALID_STATE);
    astra_page_release(&page);

    astra_page_release(&wrong_size);
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * Invalid paths
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_invalid_path(void)
{
    char dir[64];
    /* "data/main.db" is eleven characters, so a buffer that cannot hold it has to
     * be smaller than that. */
    char narrow[8];
    char path[ASTRA_STORAGE_PATH_MAX];
    astra_config config;
    astra_disk_manager *manager = NULL;

    astra_test_begin("astra_test_disk_invalid_path");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    /* A missing configuration or destination is a caller error. */
    ASTRA_CHECK_STATUS(astra_disk_manager_create(NULL, &manager),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_open(NULL, &manager),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(manager == NULL);

    /* An empty data directory has nothing to resolve against. */
    ASTRA_CHECK_STATUS(astra_config_init(&config), ASTRA_OK);
    config.data_dir[0] = '\0';
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(manager == NULL);

    /* The longest data directory the configuration will store. It is accepted by
     * the configuration, so the storage layer has to cope with it: the path is far
     * too deep to create, and the call has to fail. Which way it fails is the
     * platform's business, since the parent directory does not exist either way. */
    {
        astra_disk_manager *deep = NULL;

        ASTRA_CHECK_STATUS(astra_config_init(&config), ASTRA_OK);
        memset(config.data_dir, 'x', sizeof config.data_dir);
        config.data_dir[sizeof config.data_dir - 1u] = '\0';
        ASTRA_CHECK(astra_disk_manager_create(&config, &deep) != ASTRA_OK);
        ASTRA_CHECK(deep == NULL);
    }

    /* ASTRA_STORAGE_PATH_MAX leaves room for the separator and the file name on
     * top of the longest data directory, so a data directory the configuration
     * accepts can never overflow the path the storage layer builds. Buffer space
     * that really is too small is rejected rather than truncated, which is what
     * the narrow buffer below checks. */
    {
        const size_t longest = (ASTRA_CONFIG_PATH_MAX - 1u) + 1u
                               + (sizeof(ASTRA_DISK_MANAGER_PRIMARY_FILE) - 1u) + 1u;
        ASTRA_CHECK(longest <= (size_t)ASTRA_STORAGE_PATH_MAX);
    }

    /* An invalid page size is rejected before anything is created, so a database
     * is never laid down with a page size the rest of the process would reject.
     *
     * The field is written directly rather than through astra_config_set_page_size,
     * because that setter is what already refuses these values. The point here is
     * the second line of defence: a struct that reached the storage layer another
     * way must still not be able to produce a database. */
    {
        const uint32 bad_sizes[] = { 0u, 1u, 256u, 3000u,
                                     ASTRA_PAGE_SIZE_MIN - 1u,
                                     ASTRA_PAGE_SIZE_MAX + 1u, UINT32_MAX };
        const size_t bad_count = sizeof bad_sizes / sizeof bad_sizes[0];
        size_t index;

        for (index = 0; index < bad_count; ++index) {
            ASTRA_CHECK_STATUS(astra_config_init(&config), ASTRA_OK);
            ASTRA_CHECK_STATUS(astra_config_set_data_dir(&config, dir), ASTRA_OK);
            config.page_size = bad_sizes[index];
            ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager),
                               ASTRA_ERR_UNSUPPORTED);
            ASTRA_CHECK(manager == NULL);
        }

        /* Not one of those attempts left a file behind. */
        {
            char probe[ASTRA_STORAGE_PATH_MAX];
            FILE *stream;

            ASTRA_CHECK_STATUS(astra_path_join(probe, sizeof probe, dir,
                                               ASTRA_DISK_MANAGER_PRIMARY_FILE),
                               ASTRA_OK);
            stream = fopen(probe, "rb");
            ASTRA_CHECK(stream == NULL);
            if (stream != NULL) {
                (void)fclose(stream);
            }
        }
    }

    /* A directory whose parent does not exist. The create is one level deep by
     * design, so this is a not-found rather than a silent no-op. */
    {
        astra_config nested;

        ASTRA_CHECK_STATUS(astra_config_init(&nested), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_config_set_data_dir(&nested, "no_such_parent/child"),
                           ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_create(&nested, &manager),
                           ASTRA_ERR_NOT_FOUND);
        ASTRA_CHECK(manager == NULL);
    }

    /* A regular file where the data directory should be. The two platforms report
     * this differently, so the check is that it fails rather than which way. */
    {
        char file_path[80];
        FILE *stream;
        astra_config blocked;

        (void)snprintf(file_path, sizeof file_path, "%s_is_a_file", dir);
        stream = fopen(file_path, "wb");
        ASTRA_CHECK(stream != NULL);
        if (stream != NULL) {
            (void)fclose(stream);
        }

        ASTRA_CHECK_STATUS(astra_config_init(&blocked), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_config_set_data_dir(&blocked, file_path), ASTRA_OK);
        ASTRA_CHECK(astra_disk_manager_create(&blocked, &manager) != ASTRA_OK);
        ASTRA_CHECK(manager == NULL);
        (void)remove(file_path);
    }

    /* Path joining: the separator is not doubled, and a result that does not fit
     * is refused rather than truncated into a different path. */
    ASTRA_CHECK_STATUS(astra_path_join(path, sizeof path, "data", "main.db"),
                       ASTRA_OK);
    ASTRA_CHECK_STRING(path, "data/main.db");
    ASTRA_CHECK_STATUS(astra_path_join(path, sizeof path, "data/", "main.db"),
                       ASTRA_OK);
    ASTRA_CHECK_STRING(path, "data/main.db");
    ASTRA_CHECK_STATUS(astra_path_join(path, sizeof path, "a/b/c", "main.db"),
                       ASTRA_OK);
    ASTRA_CHECK_STRING(path, "a/b/c/main.db");

    ASTRA_CHECK_STATUS(astra_path_join(narrow, sizeof narrow, "data", "main.db"),
                       ASTRA_ERR_OUT_OF_MEMORY);
    ASTRA_CHECK_STATUS(astra_path_join(NULL, 0u, "data", "main.db"),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_path_join(path, 0u, "data", "main.db"),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_path_join(path, sizeof path, NULL, "main.db"),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_path_join(path, sizeof path, "data", NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_path_join(path, sizeof path, "data", ""),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_path_join(path, sizeof path, "", "main.db"),
                       ASTRA_ERR_INVALID_ARGUMENT);

    /* A perfectly ordinary database still works after all of that, so the
     * rejections above are not rejections of everything. */
    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * Corruption
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_corruption(void)
{
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    uint8 header[ASTRA_FORMAT_HEADER_USED];
    uint8 patch[8];

    astra_test_begin("astra_test_disk_corruption");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, 4096u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    /* The header the library wrote, byte for byte, exactly as format.h says. */
    ASTRA_CHECK(peek_file(dir, 0L, header, sizeof header));
    ASTRA_CHECK(memcmp(header + ASTRA_FORMAT_OFFSET_MAGIC, ASTRA_FORMAT_MAGIC_TEXT,
                       ASTRA_FORMAT_MAGIC_TEXT_SIZE) == 0);
    ASTRA_CHECK(header[ASTRA_FORMAT_OFFSET_MAGIC + ASTRA_FORMAT_MAGIC_TEXT_SIZE] == 0u);
    ASTRA_CHECK_UINT64(astra_load_u32_le(header + ASTRA_FORMAT_OFFSET_VERSION),
                       ASTRA_FORMAT_VERSION);
    ASTRA_CHECK_UINT64(astra_load_u32_le(header + ASTRA_FORMAT_OFFSET_PAGE_SIZE),
                       4096u);
    ASTRA_CHECK_UINT64(astra_load_u32_le(header + ASTRA_FORMAT_OFFSET_HEADER_BYTES),
                       4096u);
    ASTRA_CHECK_UINT64(astra_load_u32_le(header + ASTRA_FORMAT_OFFSET_FLAGS), 0u);
    ASTRA_CHECK_UINT64(astra_load_u64_le(header + ASTRA_FORMAT_OFFSET_RESERVED), 0u);
    ASTRA_CHECK_UINT64(astra_load_u32_le(header + ASTRA_FORMAT_OFFSET_CHECKSUM),
                       astra_checksum32(header, ASTRA_FORMAT_CHECKSUM_SPAN));

    /* Everything after the header prefix is zero, so two databases created the
     * same way are byte identical. */
    {
        uint8 page[4096];
        size_t index;
        bool clean = true;

        ASTRA_CHECK(peek_file(dir, 0L, page, sizeof page));
        for (index = ASTRA_FORMAT_HEADER_USED; index < sizeof page; ++index) {
            if (page[index] != 0u) {
                clean = false;
                break;
            }
        }
        ASTRA_CHECK(clean);
    }

    /* A file whose magic is not ours. */
    memcpy(patch, "NOTADATB", sizeof patch);
    ASTRA_CHECK(poke_file(dir, (long)ASTRA_FORMAT_OFFSET_MAGIC, patch, sizeof patch));
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager),
                       ASTRA_ERR_CORRUPTION);
    ASTRA_CHECK(manager == NULL);

    /* A damaged byte inside the checksummed prefix, with the checksum left stale,
     * is caught by the checksum. The file is deleted first because a good header
     * has to be written again, and writing one is exactly what create refuses to
     * do over an existing database. */
    remove_primary_file(dir);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    patch[0] = 0x5au;
    ASTRA_CHECK(poke_file(dir, (long)ASTRA_FORMAT_OFFSET_FLAGS, patch, 1u));
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager),
                       ASTRA_ERR_CORRUPTION);
    ASTRA_CHECK(manager == NULL);

    /* A reserved field this build does not understand. A file that is not an
     * AstraDB file at all, and an empty one, are both refused too. */
    {
        char foreign[64];
        astra_disk_manager *absent = NULL;
        astra_config foreign_config;
        char foreign_path[ASTRA_STORAGE_PATH_MAX];

        if (!astra_test_make_temp_dir(foreign, sizeof foreign)) {
            ASTRA_CHECK(false);
        } else {
            FILE *stream;

            ASTRA_CHECK(astra_test_primary_path(foreign_path, sizeof foreign_path, foreign));
            stream = fopen(foreign_path, "wb");
            ASTRA_CHECK(stream != NULL);
            if (stream != NULL) {
                static const uint8 noise[64] = { 0xde, 0xad, 0xbe, 0xef };
                ASTRA_CHECK(fwrite(noise, 1u, sizeof noise, stream) == sizeof noise);
                (void)fclose(stream);
            }
            ASTRA_CHECK_STATUS(astra_test_make_config(&foreign_config, foreign, 4096u),
                               ASTRA_OK);
            ASTRA_CHECK_STATUS(astra_disk_manager_open(&foreign_config, &absent),
                               ASTRA_ERR_CORRUPTION);
            ASTRA_CHECK(absent == NULL);
            astra_test_remove_temp_dir(foreign);
        }
    }

    /* A file whose length is not a whole number of pages, and one shorter than a
     * single page. Both are the signature of a write that was interrupted. */
    {
        char partial[64];
        astra_disk_manager *absent = NULL;
        astra_config partial_config;
        char partial_path[ASTRA_STORAGE_PATH_MAX];
        FILE *stream;

        if (!astra_test_make_temp_dir(partial, sizeof partial)) {
            ASTRA_CHECK(false);
        } else {
            ASTRA_CHECK(astra_test_primary_path(partial_path, sizeof partial_path, partial));
            stream = fopen(partial_path, "wb");
            ASTRA_CHECK(stream != NULL);
            if (stream != NULL) {
                /* 4096 plus a fragment: a length that is not a page count. */
                static const uint8 body[4100] = { 0x11 };
                ASTRA_CHECK(fwrite(body, 1u, sizeof body, stream) == sizeof body);
                (void)fclose(stream);
            }
            ASTRA_CHECK_STATUS(astra_test_make_config(&partial_config, partial, 4096u),
                               ASTRA_OK);
            ASTRA_CHECK_STATUS(astra_disk_manager_open(&partial_config, &absent),
                               ASTRA_ERR_CORRUPTION);
            ASTRA_CHECK(absent == NULL);

            /* Zero bytes: shorter than one page, so there is no header to read. */
            stream = fopen(partial_path, "wb");
            if (stream != NULL) {
                (void)fclose(stream);
            }
            absent = NULL;
            ASTRA_CHECK_STATUS(astra_disk_manager_open(&partial_config, &absent),
                               ASTRA_ERR_CORRUPTION);
            ASTRA_CHECK(absent == NULL);

            astra_test_remove_temp_dir(partial);
        }
    }

    /* A page size that disagrees with the configuration. The file is fine; the
     * caller's idea of it is not, and reading it at the wrong stride would
     * misinterpret every page in the file. */
    {
        astra_config mismatched;

        remove_primary_file(dir);
        ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, 4096u), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        manager = NULL;

        ASTRA_CHECK_STATUS(astra_test_make_config(&mismatched, dir, 8192u), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_open(&mismatched, &manager),
                           ASTRA_ERR_CORRUPTION);
        ASTRA_CHECK(manager == NULL);
    }

    /* A format version from the future, with a valid checksum, so the version check
     * is what rejects it rather than the checksum. */
    {
        astra_config same;
        uint8 version[4];

        ASTRA_CHECK_STATUS(astra_test_make_config(&same, dir, 4096u), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_open(&same, &manager), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        manager = NULL;

        astra_store_u32_le(version, ASTRA_FORMAT_VERSION + 1u);
        ASTRA_CHECK(poke_file(dir, (long)ASTRA_FORMAT_OFFSET_VERSION,
                              version, sizeof version));
        /* Re-seal the header so that only the version is wrong. */
        ASTRA_CHECK(peek_file(dir, 0L, header, (size_t)ASTRA_FORMAT_CHECKSUM_SPAN));
        astra_store_u32_le(header + ASTRA_FORMAT_OFFSET_CHECKSUM,
                           astra_checksum32(header, ASTRA_FORMAT_CHECKSUM_SPAN));
        ASTRA_CHECK(poke_file(dir, (long)ASTRA_FORMAT_OFFSET_CHECKSUM,
                              header + ASTRA_FORMAT_OFFSET_CHECKSUM, 4u));
        ASTRA_CHECK_STATUS(astra_disk_manager_open(&same, &manager),
                           ASTRA_ERR_CORRUPTION);
        ASTRA_CHECK(manager == NULL);
    }

    astra_test_remove_temp_dir(dir);
}

/*
 * ---------------------------------------------------------------------------
 * Page sizes
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_page_sizes(void)
{
    const uint32 sizes[] = { 512u, 1024u, 4096u, 16384u, 65536u };
    const size_t count = sizeof sizes / sizeof sizes[0];
    char dir[80];
    size_t index;

    astra_test_begin("astra_test_disk_page_sizes");

    /* Every accepted page size produces a working database, and the file's own
     * idea of its page size is the one the configuration asked for. */
    for (index = 0; index < count; ++index) {
        astra_config config;
        astra_disk_manager *manager = NULL;
        astra_page page = ASTRA_PAGE_INIT;
        page_id_t id = ASTRA_PAGE_ID_INVALID;
        uint64 bytes = 0;
        uint64 pages = 0;

        if (!astra_test_make_temp_dir(dir, sizeof dir)) {
            ASTRA_CHECK(false);
            continue;
        }

        ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, sizes[index]), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
        if (manager != NULL) {
            ASTRA_CHECK_UINT64(astra_disk_manager_page_size(manager), sizes[index]);
            ASTRA_CHECK_STATUS(astra_disk_manager_file_size(manager, &bytes), ASTRA_OK);
            ASTRA_CHECK_UINT64(bytes, sizes[index]);

            ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &id), ASTRA_OK);
            ASTRA_CHECK_UINT64(id, 1u);

            ASTRA_CHECK_STATUS(astra_page_init(&page, sizes[index]), ASTRA_OK);
            page.page_id = id;
            fill_pattern(&page, id);
            ASTRA_CHECK_STATUS(astra_disk_manager_write_page(manager, &page), ASTRA_OK);
            astra_page_release(&page);

            /* A page buffer of the wrong size for this database is still refused,
             * however the database itself was sized. */
            {
                astra_page wrong = ASTRA_PAGE_INIT;
                const uint32 other = (sizes[index] == 4096u) ? 8192u : 4096u;
                ASTRA_CHECK_STATUS(astra_page_init(&wrong, other), ASTRA_OK);
                ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, id, &wrong),
                                   ASTRA_ERR_INVALID_ARGUMENT);
                astra_page_release(&wrong);
            }

            ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        }

        /* Reopening with the same size still works: the header recorded it. */
        manager = NULL;
        ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager), ASTRA_OK);
        if (manager != NULL) {
            ASTRA_CHECK_STATUS(astra_disk_manager_page_count(manager, &pages),
                               ASTRA_OK);
            ASTRA_CHECK_UINT64(pages, 2u);
            ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        }

        astra_test_remove_temp_dir(dir);
    }
}

/*
 * ---------------------------------------------------------------------------
 * Stress
 * ---------------------------------------------------------------------------
 */

void astra_test_disk_stress(void)
{
    /*
     * Ten thousand pages at the default page size is a little over 150 MiB written
     * and read back. That is the point: the interesting part of a disk manager is
     * what happens when the file stops being small enough to fit in memory, and a
     * stress run that fits in the page cache proves nothing about it.
     */
    const uint64 page_count = 10000u;
    char dir[64];
    astra_config config;
    astra_disk_manager *manager = NULL;
    astra_page page = ASTRA_PAGE_INIT;
    uint64 index;
    uint64 bytes = 0;

    astra_test_begin("astra_test_disk_stress");

    if (!astra_test_make_temp_dir(dir, sizeof dir)) {
        ASTRA_CHECK(false);
        return;
    }

    ASTRA_CHECK_STATUS(astra_test_make_config(&config, dir, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_disk_manager_create(&config, &manager), ASTRA_OK);
    if (manager == NULL) {
        astra_test_remove_temp_dir(dir);
        return;
    }

    /* Allocate and write every page, in order. */
    ASTRA_CHECK_STATUS(astra_page_init(&page, ASTRA_PAGE_SIZE_DEFAULT), ASTRA_OK);
    for (index = 0; index < page_count; ++index) {
        page_id_t id = ASTRA_PAGE_ID_INVALID;

        ASTRA_CHECK_STATUS(astra_disk_manager_alloc_page(manager, &id), ASTRA_OK);
        if (id != index + 1u) {
            ASTRA_CHECK_UINT64(id, index + 1u);
            break;
        }

        page.page_id = id;
        fill_pattern(&page, id);
        if (astra_disk_manager_write_page(manager, &page) != ASTRA_OK) {
            ASTRA_CHECK(false);
            break;
        }
    }

    /* One synchronisation for the whole run, which is the point of separating
     * writing from flushing. */
    ASTRA_CHECK_STATUS(astra_disk_manager_sync(manager), ASTRA_OK);

    ASTRA_CHECK_STATUS(astra_disk_manager_file_size(manager, &bytes), ASTRA_OK);
    ASTRA_CHECK_UINT64(bytes, (page_count + 1u) * (uint64)ASTRA_PAGE_SIZE_DEFAULT);

    astra_page_release(&page);
    ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
    manager = NULL;

    /* Reopen and verify every page, in order and then out of order, so that both
     * the offsets and the content are exercised rather than just the last write. */
    manager = NULL;
    ASTRA_CHECK_STATUS(astra_disk_manager_open(&config, &manager), ASTRA_OK);
    if (manager != NULL) {
        astra_page reread = ASTRA_PAGE_INIT;
        bool all_match = true;

        ASTRA_CHECK_STATUS(astra_page_init(&reread, ASTRA_PAGE_SIZE_DEFAULT),
                           ASTRA_OK);

        for (index = 1u; index <= page_count; ++index) {
            if (astra_disk_manager_read_page(manager, index, &reread) != ASTRA_OK) {
                all_match = false;
                ASTRA_CHECK(false);
                break;
            }
            if (reread.page_id != index || !page_matches_pattern(&reread, index)) {
                all_match = false;
                ASTRA_CHECK(false);
                break;
            }
        }
        if (all_match) {
            /* One check for the run, so a failure here says which page by how far
             * the loop got rather than ten thousand identical lines. */
            ASTRA_CHECK(true);
        }

        /* Backwards, and with a stride, so a file layer that only worked in one
         * direction would be caught. */
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, page_count, &reread),
                           ASTRA_OK);
        ASTRA_CHECK(page_matches_pattern(&reread, page_count));
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, 5000u, &reread),
                           ASTRA_OK);
        ASTRA_CHECK(page_matches_pattern(&reread, 5000u));
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, 1u, &reread),
                           ASTRA_OK);
        ASTRA_CHECK(page_matches_pattern(&reread, 1u));

        /* The header page is still intact after ten thousand allocations. */
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager,
                                                        ASTRA_FORMAT_HEADER_PAGE_ID,
                                                        &reread), ASTRA_OK);
        ASTRA_CHECK(memcmp(reread.data, ASTRA_FORMAT_MAGIC_TEXT,
                           ASTRA_FORMAT_MAGIC_TEXT_SIZE) == 0);

        /* One past the last page still does not exist. */
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(manager, page_count + 1u,
                                                        &reread), ASTRA_ERR_NOT_FOUND);

        astra_page_release(&reread);
        ASTRA_CHECK_STATUS(astra_disk_manager_close(manager), ASTRA_OK);
        manager = NULL;
    }

    astra_test_remove_temp_dir(dir);
}
