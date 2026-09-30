/*
 * AstraDB storage: the on-disk format of the primary data file.
 *
 * This header is the single authoritative description of what AstraDB writes to
 * disk. It lives next to the code that writes it (`src/storage/database_file.c`)
 * rather than in `docs/`, because a layout documented in two places is a layout
 * that eventually disagrees with itself. `docs/ownership.md` explains why the
 * other ownership rules are written the way they are; the byte table below is
 * the storage analogue and is kept here for the same reason.
 *
 * The file
 * --------
 * A database is a *directory*, not a file. The directory named by
 * `astra_config::data_dir` holds every file that belongs to the database. At this
 * phase there is exactly one of them:
 *
 *     <data_dir>/main.db
 *
 * The name "main" is deliberate: it says this is the primary data file, so that a
 * later write-ahead log, index or temporary file can sit beside it without
 * renaming anything that already exists on a user's disk. The conventional
 * default path is therefore `data/main.db`.
 *
 * Pages
 * -----
 * The file is a flat array of fixed-size pages. There is no indirection, no
 * page directory and no extent map: the page with identifier N lives at byte
 * offset N * page_size, always. A page identifier is therefore stable for the
 * life of the database, which is the property every later subsystem (the buffer
 * pool's page table, the heap files, the B+Tree nodes) depends on.
 *
 * The page size is configuration, not a constant compiled into the reader. It is
 * recorded in the header page below so that a reader can discover it, and it is
 * cross-checked against the caller's configuration when a file is opened: a file
 * whose page size disagrees with the configuration is rejected rather than read
 * at the wrong stride.
 *
 * The page count
 * --------------
 * The number of pages is *derived* from the file length, not stored:
 *
 *     page_count = file_size / page_size
 *
 * A file whose length is not a whole multiple of the page size is corrupt and is
 * refused at open. Deriving the count rather than storing it is a deliberate
 * choice. A stored counter has to be updated on every allocation and stays
 * consistent with the data only if every crash window is reasoned about
 * individually; deriving it means there is exactly one source of truth and no
 * window in which two disagree. The cost is one file-size query, which the open
 * path performs anyway.
 *
 * Header page
 * -----------
 * Page 0 is reserved for database metadata and is never handed to callers as a
 * writable page. It currently holds static identity only: a magic number, the
 * format version, the page size and a checksum over them. It deliberately holds
 * no page count, no free list and no schema, because every one of those needs a
 * subsystem that does not exist yet and a speculative field is a field that has
 * to be migrated later.
 *
 * Byte layout of page 0
 * ---------------------
 * Every multi-byte field is little-endian, stored byte by byte rather than by
 * memcpy of a host integer, so the file reads identically on a big-endian host.
 * All offsets are in bytes from the start of page 0.
 *
 *   offset  size  field           meaning
 *   ------  ----  --------------  ---------------------------------------------
 *        0     8  magic           "ASTRADB" followed by one zero byte
 *       8     4  format_version  ASTRA_FORMAT_VERSION
 *      12     4  page_size       page size in bytes, must satisfy
 *                                 astra_page_size_is_valid
 *      16     4  header_bytes    bytes of page 0 reserved for this header,
 *                                 equal to page_size
 *      20     4  flags           reserved, always 0
 *      24     8  reserved        reserved, always 0
 *      32     4  checksum        CRC-32 of bytes [0, 32), little-endian
 *      36   ...  unused          zero filled to the end of the page
 *
 * `header_bytes` is redundant with `page_size` at this phase. It is kept because
 * a page layout that grows a second header region later needs an explicit length
 * rather than an implicit "up to the end of the page", and adding it now costs
 * four bytes instead of a format version bump.
 *
 * Checksums
 * ---------
 * The CRC-32 above is a CRC-32/ISO-HDLC, computed bytewise: reflected, polynomial
 * 0xEDB88320, initial value 0xFFFFFFFF, final complement. It covers only the
 * header prefix.
 *
 * It is deliberately *not* applied to data pages yet. A per-page checksum changes
 * the page layout, because each page needs a slot to keep its own checksum in, and
 * that slot is page-type specific: a heap page wants a small header, a B+Tree
 * node wants a different one. Fixing the layout before the page types exist would
 * mean either wasting the slot on every page or inventing a page header that has
 * to change later. The header checksum is worth having now because the header is
 * fixed and its fields are few; data page checksums belong with the page format.
 *
 * Durability
 * ----------
 * This format makes no transactional durability claim. `astra_disk_manager_sync`
 * asks the operating system to write pending blocks to stable storage; it does
 * not make a group of writes atomic, does not order them against anything else,
 * and does not protect against a write that is interrupted partway through a page.
 * Crash consistency is the WAL's job, and the WAL does not exist yet.
 */
#ifndef ASTRA_STORAGE_FORMAT_H
#define ASTRA_STORAGE_FORMAT_H

#include "astra/core/types.h"

/*
 * ---------------------------------------------------------------------------
 * Identity
 * ---------------------------------------------------------------------------
 */

/** Magic stored at offset 0 of the header page, without the trailing NUL. */
#define ASTRA_FORMAT_MAGIC_TEXT "ASTRADB"

/** Number of bytes occupied by the magic, including the two trailing zeros. */
#define ASTRA_FORMAT_MAGIC_SIZE 8u

/** Number of magic bytes that are the ASCII text, excluding the trailing zero. */
#define ASTRA_FORMAT_MAGIC_TEXT_SIZE 7u

/**
 * Version of the on-disk layout.
 *
 * A reader must refuse a file whose version it does not recognise. Writing a
 * future version is a deliberate act: it means every reader, including tools and
 * backups written by an older build, has been considered.
 */
#define ASTRA_FORMAT_VERSION ((uint32)1)

/** Identifier of the reserved header page. Page 0 is never a caller-owned page. */
#define ASTRA_FORMAT_HEADER_PAGE_ID ((page_id_t)0)

/*
 * ---------------------------------------------------------------------------
 * Header page field offsets
 * ---------------------------------------------------------------------------
 *
 * Exposed because a tool that reads `data/main.db` needs them, and because the
 * tests assert the bytes rather than the code that produced them.
 */

/** Offset of the 8-byte magic. */
#define ASTRA_FORMAT_OFFSET_MAGIC 0u

/** Offset of the uint32 format version. */
#define ASTRA_FORMAT_OFFSET_VERSION 8u

/** Offset of the uint32 page size. */
#define ASTRA_FORMAT_OFFSET_PAGE_SIZE 12u

/** Offset of the uint32 reserved header length. */
#define ASTRA_FORMAT_OFFSET_HEADER_BYTES 16u

/** Offset of the uint32 flags word. Always zero at this version. */
#define ASTRA_FORMAT_OFFSET_FLAGS 20u

/** Offset of the 8-byte reserved word. Always zero at this version. */
#define ASTRA_FORMAT_OFFSET_RESERVED 24u

/** Offset of the uint32 checksum. */
#define ASTRA_FORMAT_OFFSET_CHECKSUM 32u

/** Number of leading header bytes covered by the checksum, i.e. bytes [0, 32). */
#define ASTRA_FORMAT_CHECKSUM_SPAN 32u

/** Bytes of page 0 this header occupies; the remainder is zero filled. */
#define ASTRA_FORMAT_HEADER_USED 36u

/*
 * The header has to fit in the smallest page size the library accepts, otherwise
 * a file created with a small page size could not carry a header at all. Checked
 * at compile time rather than at open time, because it can never be fixed by
 * configuration.
 */
ASTRA_STATIC_ASSERT(ASTRA_FORMAT_HEADER_USED <= ASTRA_PAGE_SIZE_MIN,
                    "the header page must fit in the minimum page size");

ASTRA_STATIC_ASSERT(ASTRA_FORMAT_MAGIC_TEXT_SIZE + 1u == ASTRA_FORMAT_MAGIC_SIZE,
                    "the magic is its text followed by exactly one zero byte");

#endif /* ASTRA_STORAGE_FORMAT_H */