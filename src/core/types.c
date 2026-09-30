#include "astra/core/types.h"

/*
 * Total predicates over the identifier types. Each one answers exactly one
 * question: "is this the reserved 'nothing here' sentinel?". No other value is
 * rejected at this phase, because no other rule about these identifiers exists
 * yet. Adding rules here later is the natural place to narrow what the future
 * storage layer needs.
 */

bool astra_page_id_is_valid(page_id_t page_id)
{
    return page_id != ASTRA_PAGE_ID_INVALID;
}

bool astra_txn_id_is_valid(txn_id_t txn_id)
{
    return txn_id != ASTRA_TXN_ID_INVALID;
}

bool astra_lsn_is_valid(lsn_t lsn)
{
    return lsn != ASTRA_LSN_INVALID;
}

bool astra_page_size_is_valid(uint32 page_size)
{
    if (page_size < ASTRA_PAGE_SIZE_MIN || page_size > ASTRA_PAGE_SIZE_MAX) {
        return false;
    }
    return (page_size % ASTRA_PAGE_SIZE_MIN) == 0;
}