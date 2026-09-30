/*
 * Private helpers shared by AstraDB core translation units.
 *
 * This header is not installed and is not part of the public API. It is
 * reachable only from inside src/, and from the unit tests, which link against
 * the static library directly.
 */
#ifndef ASTRA_CORE_ASTRA_INTERNAL_H
#define ASTRA_CORE_ASTRA_INTERNAL_H

#include "astra/core/types.h"

/**
 * Marks a parameter as deliberately unused.
 *
 * Written so that the expression is still type checked: `ASTRA_UNUSED(x)` still
 * uses `x`. Wrapping in `(void)` alone also silences the warning, but this form
 * survives a future -Wunused-but-set-variable style check.
 */
#define ASTRA_UNUSED(value) ((void)(value))

/*
 * Branch hints. Purely advisory; they must never change behaviour, so on
 * compilers without the builtin they expand to the condition itself.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define ASTRA_LIKELY(condition) __builtin_expect(!!(condition), 1)
#  define ASTRA_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#else
#  define ASTRA_LIKELY(condition) (!!(condition))
#  define ASTRA_UNLIKELY(condition) (!!(condition))
#endif

/** Name reported by an astra_error that does not name its own subsystem. */
#define ASTRA_SUBSYSTEM_DEFAULT "core"

#endif /* ASTRA_CORE_ASTRA_INTERNAL_H */