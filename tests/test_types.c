#include "test_support.h"

void astra_test_type_widths(void)
{
    astra_test_begin("astra_test_type_widths");

    /*
     * The same facts are asserted at compile time by the _Static_assert block in
     * types.h. Repeating them here keeps the guarantee visible in test output,
     * and catches a toolchain whose stdint.h disagrees with the header's.
     */

    ASTRA_CHECK(sizeof(uint8) == 1);
    ASTRA_CHECK(sizeof(uint16) == 2);
    ASTRA_CHECK(sizeof(uint32) == 4);
    ASTRA_CHECK(sizeof(uint64) == 8);

    ASTRA_CHECK(sizeof(int8) == 1);
    ASTRA_CHECK(sizeof(int16) == 2);
    ASTRA_CHECK(sizeof(int32) == 4);
    ASTRA_CHECK(sizeof(int64) == 8);

    ASTRA_CHECK(sizeof(uintmax) >= sizeof(uint64));
    ASTRA_CHECK(sizeof(intmax) >= sizeof(int64));

    /* The aliases must be the stdint types, not distinct types. */
    ASTRA_CHECK(sizeof(uint8) == sizeof(uint8_t));
    ASTRA_CHECK(sizeof(uint32) == sizeof(uint32_t));
    ASTRA_CHECK(sizeof(int64) == sizeof(int64_t));

    /* Signedness is part of the contract, so check it rather than assume it. */
    ASTRA_CHECK((uint32)-1 > 0);
    ASTRA_CHECK((int32)-1 < 0);

    ASTRA_CHECK(sizeof(page_id_t) == 8);
    ASTRA_CHECK(sizeof(txn_id_t) == 8);
    ASTRA_CHECK(sizeof(lsn_t) == 8);
}

void astra_test_type_predicates(void)
{
    astra_test_begin("astra_test_type_predicates");

    /* Only the reserved sentinel is rejected; no other rule exists yet. */
    ASTRA_CHECK(astra_page_id_is_valid(0));
    ASTRA_CHECK(astra_page_id_is_valid(1));
    ASTRA_CHECK(astra_page_id_is_valid(ASTRA_PAGE_ID_INVALID - 1));
    ASTRA_CHECK(!astra_page_id_is_valid(ASTRA_PAGE_ID_INVALID));

    ASTRA_CHECK(astra_txn_id_is_valid(0));
    ASTRA_CHECK(astra_txn_id_is_valid(ASTRA_TXN_ID_INVALID - 1));
    ASTRA_CHECK(!astra_txn_id_is_valid(ASTRA_TXN_ID_INVALID));

    ASTRA_CHECK(astra_lsn_is_valid(0));
    ASTRA_CHECK(astra_lsn_is_valid(ASTRA_LSN_INVALID - 1));
    ASTRA_CHECK(!astra_lsn_is_valid(ASTRA_LSN_INVALID));

    /*
     * All three sentinels share the value UINT64_MAX. That is deliberate: "every
     * bit set means invalid" is one rule rather than three, and the three C types
     * already prevent a page identifier from being read as a log sequence
     * number. Distinct C types are the real guarantee here, not distinct values.
     */
    ASTRA_CHECK(ASTRA_PAGE_ID_INVALID == UINT64_MAX);
    ASTRA_CHECK(ASTRA_TXN_ID_INVALID == UINT64_MAX);
    ASTRA_CHECK(ASTRA_LSN_INVALID == UINT64_MAX);

    /* The largest valid value must therefore be one below the sentinel. */
    ASTRA_CHECK(astra_page_id_is_valid(UINT64_MAX - 1u));
    ASTRA_CHECK(astra_txn_id_is_valid(UINT64_MAX - 1u));
    ASTRA_CHECK(astra_lsn_is_valid(UINT64_MAX - 1u));
}

void astra_test_page_size_validity(void)
{
    astra_test_begin("astra_test_page_size_validity");

    ASTRA_CHECK(astra_page_size_is_valid(ASTRA_PAGE_SIZE_MIN));
    ASTRA_CHECK(astra_page_size_is_valid(1024));
    ASTRA_CHECK(astra_page_size_is_valid(ASTRA_PAGE_SIZE_DEFAULT));
    ASTRA_CHECK(astra_page_size_is_valid(ASTRA_PAGE_SIZE_MAX));

    ASTRA_CHECK(!astra_page_size_is_valid(0));
    ASTRA_CHECK(!astra_page_size_is_valid(1));
    ASTRA_CHECK(!astra_page_size_is_valid(ASTRA_PAGE_SIZE_MIN - 1));
    ASTRA_CHECK(!astra_page_size_is_valid(ASTRA_PAGE_SIZE_MAX + 1));

    /* The rule is a multiple of the minimum, not a power of two, at this phase.
     * 1536 is deliberately accepted so that a later tightening of the rule is a
     * visible behaviour change rather than a silent one. */
    ASTRA_CHECK(astra_page_size_is_valid(1536));
    ASTRA_CHECK(!astra_page_size_is_valid(ASTRA_PAGE_SIZE_MIN + 1));

    /* The default has to satisfy the predicate the configuration uses. */
    ASTRA_CHECK(astra_page_size_is_valid(ASTRA_PAGE_SIZE_DEFAULT));
}