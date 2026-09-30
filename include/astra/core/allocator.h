/*
 * AstraDB core: allocation abstraction.
 *
 * Every heap allocation performed by AstraDB goes through this header, never
 * through <stdlib.h> directly. That rule is what makes the allocator replaceable
 * later (a pool, an arena, a counted allocator for diagnostics) without hunting
 * through the tree.
 *
 * Ownership
 * ---------
 * The rules are uniform and are the whole point of this header:
 *
 * - `astra_alloc`, `astra_alloc_zeroed`, `astra_realloc`, `astra_strdup` and
 *   `astra_strndup` return memory owned by the caller. The caller must release
 *   it with `astra_dealloc`, never with `free`, and exactly once.
 * - `astra_dealloc(NULL)` is a no-op, so `astra_dealloc(f())` in cleanup paths
 *   is safe.
 * - No function in this header stores any of the pointers it is given, or the
 *   pointers it returns, beyond the duration of the call. The allocator holds no
 *   references to caller memory.
 * - Nothing here allocates implicitly on behalf of the caller: there are no
 *   calls that allocate hidden memory.
 *
 * Size rules
 * ----------
 * `astra_alloc(0)` and `astra_alloc_zeroed(0, n)` return NULL without recording
 * an allocation. This is a deliberate normalisation of <stdlib.h>, where both
 * zero sized allocations are permitted to succeed with an unusable pointer.
 * Callers therefore never have to distinguish "empty" from "failed" for a zero
 * size.
 */
#ifndef ASTRA_CORE_ALLOCATOR_H
#define ASTRA_CORE_ALLOCATOR_H

#include "astra/core/error.h"
#include "astra/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ---------------------------------------------------------------------------
 * Replacement seam
 * ---------------------------------------------------------------------------
 */

/**
 * The set of heap operations the library delegates to.
 *
 * A replacement must provide all four members. Once installed it must remain
 * valid until it is uninstalled again, because AstraDB calls into it from every
 * allocation site including error paths.
 *
 * Members receive the same arguments as the C library functions of the same
 * name, with one exception: a size of 0 is never passed down, because AstraDB
 * normalises zero sized allocations to NULL before delegating.
 *
 * `reallocate` is required rather than optional even though AstraDB could
 * emulate it as allocate + copy + release. A correct resize has to preserve the
 * smaller of the old and new sizes, and that old size is only known when debug
 * tracking is compiled in; in a release build an emulated resize would either
 * have to copy a length it cannot compute or silently corrupt memory. Asking for
 * four functions is a smaller cost than that.
 */
typedef struct astra_allocator_ops {
    /** Returns `size` uninitialised bytes, or NULL on failure. Must not be NULL. */
    void *(*allocate)(size_t size);

    /** Returns `count * size` zeroed bytes, or NULL on failure or overflow. Must not be NULL. */
    void *(*allocate_zeroed)(size_t count, size_t size);

    /**
     * Resizes `ptr`, preserving the smaller of the old and new sizes, or returns
     * NULL on failure leaving `ptr` untouched and still owned by the caller.
     * Must not be NULL. `ptr` is never NULL when this is called, and never has
     * size 0.
     */
    void *(*reallocate)(void *ptr, size_t size);

    /** Releases memory previously returned by `allocate`. Must not be NULL. */
    void (*deallocate)(void *ptr);
} astra_allocator_ops;

/**
 * Installs a replacement allocator, or restores the default one.
 *
 * This is process wide global state. It exists as a single, documented
 * exception to the project's "no hidden global mutable state" rule, and the
 * exception is justified because a process cannot sensibly have two allocators
 * at once. It is not thread safe: install the allocator during single threaded
 * startup, before any other AstraDB entry point runs, and do not change it
 * while allocations are in flight. Replacing the allocator invalidates every
 * pointer obtained from the previous allocator, so all such pointers must be
 * released first.
 *
 * Parameters:
 *   ops - the replacement, or NULL to restore the default <stdlib.h>
 *         allocator. NULL is allowed.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `ops` is non-NULL and any of its four members
 *   is NULL. The previously installed allocator is left in place.
 *
 * Ownership: the library does not copy `ops`; it retains the caller's pointer
 * for as long as it is installed. The caller must keep it alive and must not
 * modify it while installed. Passing NULL restores the default and releases
 * that requirement.
 */
astra_status astra_allocator_set_ops(const astra_allocator_ops *ops);

/**
 * Returns the allocator currently in effect.
 *
 * Returns: a pointer to the installed table, or to the static default table.
 * The returned pointer must not be freed or modified. It is never NULL.
 * Never fails.
 */
const astra_allocator_ops *astra_allocator_get_ops(void);

/**
 * Returns true when the default <stdlib.h> allocator is in effect.
 *
 * Never fails and allocates nothing.
 */
bool astra_allocator_is_default(void);

/*
 * ---------------------------------------------------------------------------
 * Allocation
 * ---------------------------------------------------------------------------
 */

/**
 * Allocates `size` uninitialised bytes.
 *
 * Parameters:
 *   size - number of bytes. 0 yields NULL. Values above SIZE_MAX cannot occur
 *          because size is already a size_t.
 *
 * Returns:
 *   A pointer to `size` uninitialised bytes, owned by the caller, or NULL if
 *   `size` is 0 or the allocation failed. The memory is aligned suitably for
 *   any object (max_align_t).
 *
 * Errors: reports failure only through a NULL return; see astra_error_make to
 * turn that into ASTRA_ERR_OUT_OF_MEMORY at the call site. There is no way to
 * distinguish a zero size from an exhausted heap by looking at the return value.
 */
void *astra_alloc(size_t size);

/**
 * Allocates `count * size` zeroed bytes.
 *
 * Parameters:
 *   count - number of elements. 0 yields NULL.
 *   size  - size of one element in bytes. 0 yields NULL.
 *
 * Returns:
 *   A pointer to `count * size` zeroed bytes, owned by the caller, or NULL if
 *   either argument is 0, if the multiplication would overflow size_t, or if
 *   the allocation failed. The overflow check makes a wrapped, undersized
 *   buffer impossible.
 */
void *astra_alloc_zeroed(size_t count, size_t size);

/**
 * Changes the size of an existing allocation, preserving its contents.
 *
 * Parameters:
 *   ptr  - pointer from astra_alloc, astra_alloc_zeroed or astra_strdup. NULL
 *          is allowed and makes the call equivalent to astra_alloc(size).
 *   size - new size in bytes. 0 releases `ptr` and returns NULL.
 *
 * Returns:
 *   A pointer to `size` bytes owned by the caller, or NULL on failure. On
 *   failure `ptr` is still valid and still owned by the caller, so the caller
 *   must not retry blindly. Any bytes between the old and the new size are
 *   uninitialised. When `ptr` is non-NULL and `size` is 0 the behaviour is
 *   defined (free and return NULL) rather than the implementation defined
 *   behaviour of C17 realloc(p, 0).
 */
void *astra_realloc(void *ptr, size_t size);

/**
 * Releases memory obtained from this header.
 *
 * Parameters:
 *   ptr - pointer from astra_alloc, astra_alloc_zeroed, astra_realloc,
 *         astra_strdup or astra_strndup. NULL is allowed and does nothing.
 *
 * Ownership: `ptr` is consumed. The caller must not use it afterwards and must
 * not pass it to free. Releasing a pointer twice, or a pointer that did not
 * come from this allocator, is undefined behaviour.
 */
void astra_dealloc(void *ptr);

/**
 * Duplicates a NUL terminated string.
 *
 * Parameters:
 *   text - string to copy. NULL is allowed and yields NULL.
 *
 * Returns: a NUL terminated copy owned by the caller, to be released with
 * astra_dealloc, or NULL if `text` is NULL or the allocation failed. The result
 * is never NULL when `text` is not NULL and allocation succeeded, so a non-NULL
 * result always has a well defined length.
 */
char *astra_strdup(const char *text);

/**
 * Duplicates at most `length` bytes of a string, always NUL terminating.
 *
 * Useful for copying a fixed width, possibly unterminated, on-disk field.
 *
 * Parameters:
 *   text   - bytes to copy. NULL is allowed and yields NULL.
 *   length - maximum number of bytes to read from `text`. 0 yields an empty
 *            string, which is still a valid, non-NULL, owned pointer.
 *
 * Returns: a NUL terminated copy of at most `length` bytes, owned by the
 * caller, or NULL if `text` is NULL or the allocation failed. Copying stops at
 * the first NUL in `text`, so the result is never longer than the source's
 * logical string.
 */
char *astra_strndup(const char *text, size_t length);

/*
 * ---------------------------------------------------------------------------
 * Debug accounting
 * ---------------------------------------------------------------------------
 *
 * Tracking is compiled in only when NDEBUG is not defined, which is exactly the
 * Debug and RelWithDebInfo-with-assertions configurations. In a release build
 * the counters report zeros and astra_allocator_tracking_enabled returns false.
 * The release path performs no extra work at all: there is no header, no
 * counter and no branch.
 *
 * Tracking exists to make leaks observable in tests, not to be a profiler. It
 * is not a pool and it changes no allocation behaviour.
 */

/** Snapshot of the allocator's live allocation counters. */
typedef struct astra_alloc_stats {
    /** Blocks currently allocated and not yet released. */
    uint64 live_blocks;

    /** Payload bytes currently allocated, excluding allocator bookkeeping. */
    uint64 live_bytes;

    /** Total successful allocations since process start, including live ones. */
    uint64 total_allocations;

    /** Total releases since process start, including ones that freed nothing. */
    uint64 total_deallocations;
} astra_alloc_stats;

/**
 * Returns true when live allocation tracking is compiled in.
 *
 * Never fails and allocates nothing.
 */
bool astra_allocator_tracking_enabled(void);

/**
 * Copies the current counters into `out`.
 *
 * Parameters:
 *   out - destination. Must not be NULL.
 *
 * Returns: always ASTRA_OK; present so that future implementations can report
 * a failure. Never allocates.
 */
astra_status astra_allocator_stats(astra_alloc_stats *out);

/**
 * Returns the number of payload bytes currently allocated.
 *
 * Returns 0 when tracking is disabled. Never fails and allocates nothing.
 */
uint64 astra_allocator_live_bytes(void);

/**
 * Returns the number of blocks currently allocated.
 *
 * Returns 0 when tracking is disabled. Never fails and allocates nothing.
 */
uint64 astra_allocator_live_blocks(void);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_CORE_ALLOCATOR_H */