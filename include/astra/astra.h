#ifndef ASTRA_ASTRA_H
#define ASTRA_ASTRA_H

/*
 * Umbrella header for the public AstraDB C API.
 *
 * Including this header brings in every public module. Code that depends on a
 * single module may include its header directly instead; both are supported and
 * the sub-headers are self contained.
 *
 * Ownership
 * ---------
 * Ownership rules are not repeated here, because a header that restates them
 * goes stale. Each module documents its own:
 *
 *   astra/core/types.h      fixed width aliases, page_id_t, txn_id_t, lsn_t
 *   astra/core/error.h      status codes, categories, astra_error
 *   astra/core/allocator.h  the heap; every allocation goes through it
 *   astra/core/log.h        logging
 *   astra/core/config.h     process configuration
 *
 * See docs/ownership.md for the rules those headers share.
 */

#include "astra/core/allocator.h"
#include "astra/core/config.h"
#include "astra/core/error.h"
#include "astra/core/log.h"
#include "astra/core/types.h"
#include "astra/version.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns the library version as "MAJOR.MINOR.PATCH".
 *
 * The returned string is a static, NUL terminated, immutable buffer owned by
 * the library. It is valid for the lifetime of the process and must not be
 * modified or freed by the caller.
 */
const char *astra_version(void);

/**
 * Initializes the library.
 *
 * Returns ASTRA_OK on success, or ASTRA_ERR_ALREADY_INITIALIZED if the library
 * is already initialized.
 *
 * The core subsystems (allocator, logging, error reporting) are usable without
 * calling this first; it marks the start of a database session and exists so
 * that later phases have somewhere to hang session wide setup. astra_config is
 * not applied automatically: the caller builds the configuration it wants and
 * passes the settings it cares about to the subsystem that consumes them, for
 * example astra_log_set_level.
 *
 * Performs no allocation.
 */
astra_status astra_init(void);

/**
 * Shuts the library down, undoing astra_init().
 *
 * Returns ASTRA_OK on success, or ASTRA_ERR_NOT_INITIALIZED if the library was
 * not initialized.
 *
 * Performs no allocation and releases nothing: the core subsystems hold no
 * resources at this phase.
 */
astra_status astra_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_ASTRA_H */
