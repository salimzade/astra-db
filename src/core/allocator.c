#include "astra/core/allocator.h"

#include "core/astra_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/*
 * AstraDB's heap.
 *
 * Two things live here and nothing else:
 *
 *   1. A delegation table, so the C library can be swapped out wholesale later.
 *   2. Debug only accounting, so tests can prove that every allocation was
 *      released.
 *
 * Neither is a pool. There is no free list, no size class and no bump
 * allocator; a real arena belongs with the buffer manager, which will know how
 * big its blocks should be, and writing one now would be guessing.
 */

/*
 * ---------------------------------------------------------------------------
 * Debug accounting
 * ---------------------------------------------------------------------------
 *
 * Each tracked block carries a header holding its payload size. The header is
 * what makes live_bytes exact.
 *
 * The header is best effort bookkeeping, not an integrity check. It deliberately
 * does not carry a magic word or attempt to detect a foreign or double pointer:
 * validating a pointer that did not come from this allocator would require a
 * registry of every live block, and acting on a failed check would mean either
 * freeing memory we do not own or aborting the process. Both are worse problems
 * than the one they would pretend to solve. Corruption is caught with
 * AddressSanitizer, which is the right tool.
 *
 * The union with max_align_t guarantees that the payload starts at an address
 * suitable for any object, including over-aligned ones, regardless of how the
 * header itself happened to be aligned.
 */
#if defined(NDEBUG)
#  define ASTRA_ALLOC_TRACKING 0
#else
#  define ASTRA_ALLOC_TRACKING 1
#endif

#if ASTRA_ALLOC_TRACKING

/*
 * The header must not reduce the alignment of the payload that follows it.
 *
 * C11 gives max_align_t for exactly this, but MSVC does not provide it in C
 * mode, so the alignment is obtained from the widest scalar types directly.
 * long double is included because it is the most strictly aligned fundamental
 * type on the ABIs that matter (16 bytes on x86-64 SysV, 8 on MSVC x64).
 *
 * The union member order is irrelevant; its alignment is that of its most
 * strictly aligned member.
 */
/*
 * The widest scalar alignment the payload must preserve, expressed as a type so
 * that _Alignof can be applied to it on every compiler, including those without
 * max_align_t.
 */
typedef union {
    long double value;
    void *pointer;
    uint64 integer;
} max_align_t_fallback;

typedef union astra_alloc_header {
    struct {
        size_t size;
        size_t reserved;
    } fields;
    long double align_long_double;
    void *align_pointer;
    uint64 align_uint64;
} astra_alloc_header;

ASTRA_STATIC_ASSERT(_Alignof(astra_alloc_header) >= _Alignof(max_align_t_fallback),
                    "the allocation header must be at least as aligned as the widest scalar");

ASTRA_STATIC_ASSERT(sizeof(astra_alloc_header) % _Alignof(astra_alloc_header) == 0,
                    "the allocation header size must be a multiple of its alignment");

static atomic_uint_least64_t g_live_blocks;
static atomic_uint_least64_t g_live_bytes;
static atomic_uint_least64_t g_total_allocations;
static atomic_uint_least64_t g_total_deallocations;

static void note_allocated(size_t size)
{
    atomic_fetch_add_explicit(&g_live_blocks, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_live_bytes, (uint_least64_t)size, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_total_allocations, 1u, memory_order_relaxed);
}

static void note_deallocated(size_t size)
{
    atomic_fetch_sub_explicit(&g_live_blocks, 1u, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_live_bytes, (uint_least64_t)size, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_total_deallocations, 1u, memory_order_relaxed);
}

#endif /* ASTRA_ALLOC_TRACKING */

/*
 * ---------------------------------------------------------------------------
 * The default allocator
 * ---------------------------------------------------------------------------
 */

static void *system_allocate(size_t size)
{
    return malloc(size);
}

static void *system_allocate_zeroed(size_t count, size_t size)
{
    return calloc(count, size);
}

static void *system_reallocate(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

static void system_deallocate(void *ptr)
{
    free(ptr);
}

static const astra_allocator_ops k_system_ops = {
    system_allocate,
    system_allocate_zeroed,
    system_reallocate,
    system_deallocate
};

/*
 * The installed table.
 *
 * Documented process global state; see astra_allocator_set_ops for the contract.
 * It is read on every allocation, and deliberately not atomic: installing an
 * allocator while other threads are allocating is a programming error however
 * the load happens to be spelled.
 */
static const astra_allocator_ops *g_ops = &k_system_ops;

/*
 * ---------------------------------------------------------------------------
 * Size arithmetic
 * ---------------------------------------------------------------------------
 */

/*
 * Returns count * size, or 0 if the product would overflow.
 *
 * Zero is the caller's signal to return NULL. It is deliberately also the result
 * for a genuinely empty product: both mean "there is nothing to hand back", and
 * in neither case may the multiplication be allowed to wrap into a plausible but
 * undersized buffer.
 */
static size_t checked_product(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        return 0;
    }
    if (count > SIZE_MAX / size) {
        return 0;
    }
    return count * size;
}

/*
 * ---------------------------------------------------------------------------
 * Raw delegation
 * ---------------------------------------------------------------------------
 *
 * These two are the only places in AstraDB that touch the installed table. The
 * debug header is applied here and nowhere else, so every caller above sees one
 * uniform contract regardless of build configuration.
 */

static void *allocate_delegate(const astra_allocator_ops *ops, size_t total, bool zeroed)
{
#if ASTRA_ALLOC_TRACKING
    void *raw = zeroed ? ops->allocate_zeroed(1, total + sizeof(astra_alloc_header))
                       : ops->allocate(total + sizeof(astra_alloc_header));

    if (raw == NULL) {
        return NULL;
    }

    {
        astra_alloc_header *header = (astra_alloc_header *)raw;

        header->fields.size = total;
        header->fields.reserved = 0;
        note_allocated(total);
        return (void *)(header + 1);
    }
#else
    return zeroed ? ops->allocate_zeroed(1, total) : ops->allocate(total);
#endif
}

static void deallocate_delegate(const astra_allocator_ops *ops, void *ptr)
{
#if ASTRA_ALLOC_TRACKING
    astra_alloc_header *header = ((astra_alloc_header *)ptr) - 1;

    note_deallocated(header->fields.size);
    ops->deallocate(header);
#else
    ops->deallocate(ptr);
#endif
}

/*
 * ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------
 */

astra_status astra_allocator_set_ops(const astra_allocator_ops *ops)
{
    if (ops != NULL) {
        /* All four members are required; see the comment on the type. A table
         * with a hole in it would otherwise be installed and crash later, on
         * some unrelated call, at which point the cause is far away. */
        if (ops->allocate == NULL || ops->allocate_zeroed == NULL ||
            ops->reallocate == NULL || ops->deallocate == NULL) {
            return ASTRA_ERR_INVALID_ARGUMENT;
        }
    }

    g_ops = (ops != NULL) ? ops : &k_system_ops;
    return ASTRA_OK;
}

const astra_allocator_ops *astra_allocator_get_ops(void)
{
    return g_ops;
}

bool astra_allocator_is_default(void)
{
    return g_ops == &k_system_ops;
}

void *astra_alloc(size_t size)
{
    if (ASTRA_UNLIKELY(size == 0)) {
        return NULL;
    }
    return allocate_delegate(g_ops, size, false);
}

void *astra_alloc_zeroed(size_t count, size_t size)
{
    size_t total = checked_product(count, size);

    if (total == 0) {
        return NULL;
    }
    return allocate_delegate(g_ops, total, true);
}

void *astra_realloc(void *ptr, size_t size)
{
    const astra_allocator_ops *ops = g_ops;

    if (ptr == NULL) {
        return astra_alloc(size);
    }
    if (size == 0) {
        /* Defined here rather than left to C17's implementation defined
         * realloc(p, 0), which may either free or return a unique pointer. */
        astra_dealloc(ptr);
        return NULL;
    }

#if ASTRA_ALLOC_TRACKING
    {
        astra_alloc_header *header = ((astra_alloc_header *)ptr) - 1;
        size_t old_size = header->fields.size;
        void *fresh = allocate_delegate(ops, size, false);

        if (fresh != NULL) {
            memcpy(fresh, ptr, (old_size < size) ? old_size : size);
            deallocate_delegate(ops, ptr);
        }
        return fresh;
    }
#else
    return ops->reallocate(ptr, size);
#endif
}

void astra_dealloc(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    deallocate_delegate(g_ops, ptr);
}

char *astra_strdup(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }

    length = strlen(text);

    /* +1 for the NUL. An empty string still needs a valid, owned, one byte
     * allocation so that callers can free the result unconditionally. */
    copy = (char *)astra_alloc(length + 1u);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length + 1u);
    return copy;
}

char *astra_strndup(const char *text, size_t length)
{
    char *copy;
    size_t actual;

    if (text == NULL) {
        return NULL;
    }

    if (length == 0) {
        /* Still produce an owned empty string, so that the result is always
         * freeable and the caller never needs a special case for "no bytes". */
        copy = (char *)astra_alloc(1u);
        if (copy != NULL) {
            copy[0] = '\0';
        }
        return copy;
    }

    /* strnlen semantics without depending on strnlen, which POSIX has and C17
     * does not. Stopping at the source NUL means we cannot overrun the source,
     * and `actual` then excludes the NUL, so it is never copied twice. */
    actual = 0;
    while (actual < length && text[actual] != '\0') {
        ++actual;
    }

    copy = (char *)astra_alloc(actual + 1u);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, actual);
    copy[actual] = '\0';
    return copy;
}

bool astra_allocator_tracking_enabled(void)
{
#if ASTRA_ALLOC_TRACKING
    return true;
#else
    return false;
#endif
}

astra_status astra_allocator_stats(astra_alloc_stats *out)
{
    if (out == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

#if ASTRA_ALLOC_TRACKING
    out->live_blocks = (uint64)atomic_load_explicit(&g_live_blocks, memory_order_relaxed);
    out->live_bytes = (uint64)atomic_load_explicit(&g_live_bytes, memory_order_relaxed);
    out->total_allocations =
        (uint64)atomic_load_explicit(&g_total_allocations, memory_order_relaxed);
    out->total_deallocations =
        (uint64)atomic_load_explicit(&g_total_deallocations, memory_order_relaxed);
#else
    out->live_blocks = 0;
    out->live_bytes = 0;
    out->total_allocations = 0;
    out->total_deallocations = 0;
#endif

    return ASTRA_OK;
}

uint64 astra_allocator_live_bytes(void)
{
#if ASTRA_ALLOC_TRACKING
    return (uint64)atomic_load_explicit(&g_live_bytes, memory_order_relaxed);
#else
    return 0;
#endif
}

uint64 astra_allocator_live_blocks(void)
{
#if ASTRA_ALLOC_TRACKING
    return (uint64)atomic_load_explicit(&g_live_blocks, memory_order_relaxed);
#else
    return 0;
#endif
}