/*
 * Private seam over the logging sink.
 *
 * The public API hard codes "write one formatted line to stderr". That is the
 * right default for a database process, but it also makes the logger untestable:
 * a unit test cannot assert on output that goes to the terminal, and it cannot
 * prove that a TRACE record was suppressed.
 *
 * Rather than publish a sink API that callers would only misuse, the seam lives
 * here. Tests include this header directly; the rest of the tree does not.
 *
 * Ownership: the installed function and its context must remain valid until they
 * are replaced. They are set before the logger is used concurrently and are not
 * touched during steady state.
 */
#ifndef ASTRA_CORE_LOG_INTERNAL_H
#define ASTRA_CORE_LOG_INTERNAL_H

#include "astra/core/types.h"

/**
 * Receives one fully formatted log line, NUL terminated.
 *
 * `line` is owned by the logger and is only valid for the duration of the call.
 * `length` excludes the terminating NUL and is always less than or equal to
 * ASTRA_LOG_LINE_MAX.
 */
typedef void (*astra_log_sink_fn)(void *context, const char *line, size_t length);

/**
 * Installs the destination for formatted lines, or restores stderr.
 *
 * Parameters:
 *   sink    - destination, or NULL to restore the default stderr sink. NULL is
 *             allowed.
 *   context - opaque value passed back to `sink`. Ignored when `sink` is NULL.
 *
 * Thread safety: not thread safe. Call during single threaded test setup or
 * before any concurrent logging starts.
 *
 * Ownership: neither the function nor the context is copied; both must outlive
 * their installation. Passing NULL releases that requirement.
 */
void astra_log_internal_set_sink(astra_log_sink_fn sink, void *context);

/**
 * Returns true while a sink other than the default stderr sink is installed.
 *
 * Exists so that tests can assert they are not observing each other's state.
 * Never fails and allocates nothing.
 */
bool astra_log_internal_has_custom_sink(void);

#endif /* ASTRA_CORE_LOG_INTERNAL_H */