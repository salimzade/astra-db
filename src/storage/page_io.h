/*
 * AstraDB storage: the page level seam between the Disk Manager and the file.
 *
 * Private. page_io.c is deliberately the smallest of the three storage modules.
 * Its whole job is the question "was this request about a page sensible?", which
 * neither of the layers on either side of it is in a position to answer:
 *
 *   - database_file.c cannot know that a buffer is the wrong size or that the
 *     caller asked for the header page.
 *   - disk_manager.c knows the configuration, but if it also did the per-call
 *     validation the two modules would duplicate the same ten lines of checks and
 *     drift apart the first time one of them was edited.
 *
 * So the rule lives here: this module validates page identifiers, page buffers and
 * buffer sizes, and hands the file layer a request it has already proved well
 * formed. It does not allocate, does not log, and does not synchronise.
 */
#ifndef ASTRA_STORAGE_PAGE_IO_H
#define ASTRA_STORAGE_PAGE_IO_H

#include "storage/database_file.h"

#include "astra/core/error.h"
#include "astra/core/types.h"
#include "astra/storage/page.h"

/**
 * Reads one whole page from `file` into `page`.
 *
 * Validates `page` against `file` (initialised, and `data_size` equal to the
 * file's page size) and `page_id` against the file's real length, then reads
 * exactly one page. `page->page_id` and `page->is_dirty` are set only after the
 * read succeeds.
 */
astra_status astra_page_io_read(astra_database_file *file,
                                page_id_t page_id,
                                astra_page *page);

/**
 * Writes one whole page from `page` into `file`.
 *
 * Validates as astra_page_io_read does, refuses the header page, and clears
 * `page->is_dirty` only after the write succeeds.
 *
 * `page` is not const because a successful write clears its dirty flag; see the
 * implementation for why that is not left to the caller.
 */
astra_status astra_page_io_write(astra_database_file *file, astra_page *page);

#endif /* ASTRA_STORAGE_PAGE_IO_H */