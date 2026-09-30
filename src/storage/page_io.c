#include "storage/page_io.h"

#include "astra/core/allocator.h"
#include "astra/core/log.h"
#include "astra/storage/format.h"
#include "storage/storage_internal.h"

#include <string.h>

/*
 * The page level seam.
 *
 * Every rule about what a caller may ask for, expressed once:
 *
 *   - a page buffer must exist and must be exactly this file's page size;
 *   - a page identifier must address a page the file actually has;
 *   - the header page may be read and may not be written.
 *
 * None of these can be checked lower down. The file layer has no idea what size a
 * caller's buffer is meant to be, and the Disk Manager would end up restating all
 * three if this module did not. There is no I/O in this file beyond delegating to
 * astra_file_read_page and astra_file_write_page, and nothing here allocates except
 * the page buffer itself.
 */

/*
 * ---------------------------------------------------------------------------
 * Page buffers
 * ---------------------------------------------------------------------------
 */

astra_status astra_page_init(astra_page *page, uint32 data_size)
{
    void *buffer;

    if (page == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (!astra_page_size_is_valid(data_size)) {
        return ASTRA_ERR_UNSUPPORTED;
    }

    buffer = astra_alloc((size_t)data_size);
    if (buffer == NULL) {
        return ASTRA_ERR_OUT_OF_MEMORY;
    }
    memset(buffer, 0, (size_t)data_size);

    page->page_id = ASTRA_PAGE_ID_INVALID;
    page->data_size = data_size;
    page->data = buffer;
    page->is_dirty = false;

    return ASTRA_OK;
}

void astra_page_release(astra_page *page)
{
    if (page == NULL || page->data == NULL) {
        return;
    }

    astra_dealloc(page->data);

    page->page_id = ASTRA_PAGE_ID_INVALID;
    page->data_size = 0u;
    page->data = NULL;
    page->is_dirty = false;
}

bool astra_page_is_initialized(const astra_page *page)
{
    return page != NULL && page->data != NULL;
}

astra_status astra_page_clear(astra_page *page, uint32 length)
{
    if (page == NULL || page->data == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (length > page->data_size) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    memset(page->data, 0, (size_t)length);
    return ASTRA_OK;
}

/*
 * ---------------------------------------------------------------------------
 * Page transfer
 * ---------------------------------------------------------------------------
 */

/*
 * Checks that `page` is a buffer this file can be read into or written from.
 *
 * The size check is exact rather than "at least". A buffer larger than the page
 * would let a caller believe it had written or received a whole page's worth of
 * meaningful bytes and silently ignore the remainder, and a buffer smaller would be
 * an overwrite. Neither is something a database can recover from, so the only
 * acceptable answer is to refuse both.
 */
static astra_status validate_buffer(const astra_database_file *file,
                                    const astra_page *page)
{
    if (page == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (page->data == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    if (page->data_size != file->page_size) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }
    return ASTRA_OK;
}

astra_status astra_page_io_read(astra_database_file *file,
                                page_id_t page_id,
                                astra_page *page)
{
    astra_status status;

    if (file == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    status = validate_buffer(file, page);
    if (status != ASTRA_OK) {
        return status;
    }

    if (!astra_file_has_page(file, page_id)) {
        return ASTRA_ERR_NOT_FOUND;
    }

    /*
     * The bytes go straight into the caller's buffer rather than through a
     * temporary, because there is nothing to protect: the identifier has already
     * been checked against the file's length, and a transfer that cannot be
     * completed is reported rather than half-applied. What a failed read may leave
     * in the buffer is unspecified, which is why `page_id` is not updated - a caller
     * that checks the status knows not to believe the buffer, and a caller that
     * does not is no worse off than one that never checked.
     */
    status = astra_file_read_page(file, page_id, page->data, page->data_size);
    if (status != ASTRA_OK) {
        return status;
    }

    /*
     * The identifier and the dirty flag are set only once the bytes are known to be
     * in the buffer, so a read that fails leaves the page describing what it
     * described before rather than claiming to hold a page it does not.
     */
    page->page_id = page_id;
    page->is_dirty = false;

    return ASTRA_OK;
}

astra_status astra_page_io_write(astra_database_file *file, astra_page *page)
{
    astra_status status;

    if (file == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    status = validate_buffer(file, page);
    if (status != ASTRA_OK) {
        return status;
    }

    if (page->page_id == ASTRA_FORMAT_HEADER_PAGE_ID) {
        /*
         * Refused rather than honoured. A write here would overwrite the magic
         * number and the checksum, and the database could never be opened again;
         * honouring it and finding out later is strictly worse.
         */
        return ASTRA_ERR_INVALID_STATE;
    }

    if (!astra_file_has_page(file, page->page_id)) {
        return ASTRA_ERR_NOT_FOUND;
    }

    status = astra_file_write_page(file, page->page_id, page->data, page->data_size);
    if (status != ASTRA_OK) {
        return status;
    }

    /*
     * The bytes on disk now match the buffer, so the page is no longer dirty. This
     * is the one field a write is defined to change, and it is why the parameter is
     * not const: leaving a page marked dirty after its bytes are safely on disk is
     * how a later write gets skipped for no reason.
     */
    page->is_dirty = false;

    return ASTRA_OK;
}