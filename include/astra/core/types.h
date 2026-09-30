/*
 * AstraDB core: fixed width integer aliases and shared identifier types.
 *
 * This header has no dependencies beyond <stdint.h>, <stddef.h> and
 * <stdbool.h> and may be included from anywhere, including other headers.
 *
 * Ownership
 * ---------
 * Nothing in this header owns memory. Every value here is a scalar or an
 * enumeration.
 *
 * Scope of this phase
 * -------------------
 * The identifier types (page_id_t, txn_id_t, lsn_t) exist so that future
 * subsystems have a single, named, correctly sized type to build on. They
 * intentionally carry no database semantics yet: no allocation policy, no
 * ordering guarantees, no persistence format. Only their width, their signedness
 * and their "not a valid identifier" sentinel are defined here. The functions
 * in this header are therefore limited to validating the sentinel.
 */
#ifndef ASTRA_CORE_TYPES_H
#define ASTRA_CORE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile time assertion usable from public headers.
 *
 * `_Static_assert` is C11; the C++ branch exists only so that this header stays
 * syntactically valid for C++ consumers of the public API. AstraDB itself is
 * C only.
 */
#if defined(__cplusplus)
#  define ASTRA_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#  define ASTRA_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

/** Number of elements in a statically sized array. */
#define ASTRA_ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

/*
 * Fixed width integer aliases.
 *
 * These are plain aliases of the <stdint.h> types, not new types: a function
 * taking `uint32` is exactly a function taking `uint32_t`. They exist to give
 * the codebase a compact, width-explicit spelling that reads well in layout
 * definitions and arithmetic, and to make the intended width obvious at every
 * use site.
 *
 * Sizes are asserted below, so a platform where any of these differ fails at
 * compile time rather than at runtime.
 */
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;

/** Widest unsigned type guaranteed to hold any `uint8` through `uint64` value. */
typedef uint64_t uintmax;

/** Widest signed type guaranteed to hold any `int8` through `int64` value. */
typedef int64_t intmax;

ASTRA_STATIC_ASSERT(sizeof(uint8) == 1, "uint8 must be exactly one byte");
ASTRA_STATIC_ASSERT(sizeof(uint16) == 2, "uint16 must be exactly two bytes");
ASTRA_STATIC_ASSERT(sizeof(uint32) == 4, "uint32 must be exactly four bytes");
ASTRA_STATIC_ASSERT(sizeof(uint64) == 8, "uint64 must be exactly eight bytes");
ASTRA_STATIC_ASSERT(sizeof(int8) == 1, "int8 must be exactly one byte");
ASTRA_STATIC_ASSERT(sizeof(int16) == 2, "int16 must be exactly two bytes");
ASTRA_STATIC_ASSERT(sizeof(int32) == 4, "int32 must be exactly four bytes");
ASTRA_STATIC_ASSERT(sizeof(int64) == 8, "int64 must be exactly eight bytes");

/*
 * ---------------------------------------------------------------------------
 * Identifier types
 * ---------------------------------------------------------------------------
 */

/**
 * Identifier of a physical page inside the data file.
 *
 * Width and signedness are fixed at 64 bits so that a page identifier can be
 * used as an array index into a page table without further conversions, and so
 * that the on-disk identifier width can never change silently.
 *
 * No semantics are defined yet. In particular this phase does not define how
 * page identifiers are assigned, whether they are dense, or whether a
 * particular value has a special meaning beyond ASTRA_PAGE_ID_INVALID below.
 */
typedef uint64 page_id_t;

/**
 * Identifier of a transaction.
 *
 * Width and signedness are fixed at 64 bits so that transaction identifiers can
 * be compared and ordered directly, which the future MVCC visibility rules
 * will rely on.
 *
 * No semantics are defined yet. In particular this phase does not define how
 * transaction identifiers are assigned, how they relate to commit order, or
 * which range is reserved for special use beyond ASTRA_TXN_ID_INVALID below.
 */
typedef uint64 txn_id_t;

/**
 * Log sequence number: a position in the write-ahead log.
 *
 * Width and signedness are fixed at 64 bits so that log positions never wrap in
 * any practical lifetime and can be compared and subtracted directly.
 *
 * No semantics are defined yet. This phase does not assign LSNs, persist them
 * or recover from them.
 */
typedef uint64 lsn_t;

/** Sentinel meaning "no page identifier is available". */
#define ASTRA_PAGE_ID_INVALID ((page_id_t)UINT64_MAX)

/** Sentinel meaning "no transaction identifier is available". */
#define ASTRA_TXN_ID_INVALID ((txn_id_t)UINT64_MAX)

/** Sentinel meaning "no log sequence number is available". */
#define ASTRA_LSN_INVALID ((lsn_t)UINT64_MAX)

ASTRA_STATIC_ASSERT(sizeof(page_id_t) == sizeof(uint64), "page_id_t must be 64 bits");
ASTRA_STATIC_ASSERT(sizeof(txn_id_t) == sizeof(uint64), "txn_id_t must be 64 bits");
ASTRA_STATIC_ASSERT(sizeof(lsn_t) == sizeof(uint64), "lsn_t must be 64 bits");

/*
 * ---------------------------------------------------------------------------
 * Page size
 * ---------------------------------------------------------------------------
 *
 * The bounds below are the validation limits for the configurable page size.
 * They are not a storage format definition: no page layout exists yet. They
 * exist so that a nonsensical page size can be rejected at configuration time
 * instead of producing a broken data file later.
 */

/** Smallest accepted page size, in bytes. */
#define ASTRA_PAGE_SIZE_MIN ((uint32)512)

/** Largest accepted page size, in bytes. */
#define ASTRA_PAGE_SIZE_MAX ((uint32)65536)

/** Page size used when no page size is configured explicitly. */
#define ASTRA_PAGE_SIZE_DEFAULT ((uint32)4096)

/*
 * ---------------------------------------------------------------------------
 * Predicates
 * ---------------------------------------------------------------------------
 *
 * All of these are total functions: no argument is NULL and no argument can
 * fail. Out of range enum-like inputs are reported, not diagnosed.
 */

/**
 * Returns true when `page_id` is a usable page identifier.
 *
 * Parameters:
 *   page_id - any value of type page_id_t; NULL is not applicable.
 *
 * Returns: false only when `page_id` equals ASTRA_PAGE_ID_INVALID.
 * Never fails and allocates nothing.
 */
bool astra_page_id_is_valid(page_id_t page_id);

/**
 * Returns true when `txn_id` is a usable transaction identifier.
 *
 * Parameters:
 *   txn_id - any value of type txn_id_t; NULL is not applicable.
 *
 * Returns: false only when `txn_id` equals ASTRA_TXN_ID_INVALID.
 * Never fails and allocates nothing.
 */
bool astra_txn_id_is_valid(txn_id_t txn_id);

/**
 * Returns true when `lsn` is a usable log sequence number.
 *
 * Parameters:
 *   lsn - any value of type lsn_t; NULL is not applicable.
 *
 * Returns: false only when `lsn` equals ASTRA_LSN_INVALID.
 * Never fails and allocates nothing.
 */
bool astra_lsn_is_valid(lsn_t lsn);

/**
 * Returns true when `page_size` is an acceptable page size.
 *
 * An acceptable page size is a multiple of ASTRA_PAGE_SIZE_MIN that does not
 * exceed ASTRA_PAGE_SIZE_MAX. A power-of-two requirement is deliberately not
 * imposed at this phase; the storage layer may tighten this predicate.
 *
 * Parameters:
 *   page_size - byte count; NULL is not applicable.
 *
 * Returns: true when `page_size` is within [ASTRA_PAGE_SIZE_MIN,
 * ASTRA_PAGE_SIZE_MAX] and is a whole number of minimum-size units.
 * Never fails and allocates nothing.
 */
bool astra_page_size_is_valid(uint32 page_size);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_CORE_TYPES_H */