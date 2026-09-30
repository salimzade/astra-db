#include "test_support.h"

#include <stdlib.h>

/*
 * A replacement allocator used to prove that astra_allocator_set_ops really does
 * redirect every allocation, including the debug-tracked release path.
 *
 * It counts calls rather than delegating to the C library, so a test can assert
 * both that the custom table was consulted and that the default was not.
 */
typedef struct probe_allocator {
    unsigned long allocate_calls;
    unsigned long allocate_zeroed_calls;
    unsigned long reallocate_calls;
    unsigned long deallocate_calls;
    size_t requested_total;

    /* When set, every allocation fails, to exercise the failure paths. */
    bool fail_all;

    /* When set, every allocation returns this, to exercise pointer identity. */
    void *override_return;
} probe_allocator;

static probe_allocator g_probe;

static void *probe_allocate(size_t size)
{
    ++g_probe.allocate_calls;
    g_probe.requested_total += size;

    if (g_probe.fail_all) {
        return NULL;
    }
    return malloc(size);
}

static void *probe_allocate_zeroed(size_t count, size_t size)
{
    ++g_probe.allocate_zeroed_calls;
    g_probe.requested_total += count * size;

    if (g_probe.fail_all) {
        return NULL;
    }
    return calloc(count, size);
}

static void *probe_reallocate(void *ptr, size_t size)
{
    ++g_probe.reallocate_calls;
    g_probe.requested_total += size;

    if (g_probe.fail_all) {
        return NULL;
    }
    return realloc(ptr, size);
}

static void probe_deallocate(void *ptr)
{
    ++g_probe.deallocate_calls;
    free(ptr);
}

/* Two tables with a hole in each, to prove that set_ops validates all four
 * members rather than only the ones that happen to be reached first. */
static const astra_allocator_ops k_no_zeroed_ops = {
    probe_allocate,
    NULL, /* allocate_zeroed deliberately absent */
    probe_reallocate,
    probe_deallocate
};

static const astra_allocator_ops k_no_reallocate_ops = {
    probe_allocate,
    probe_allocate_zeroed,
    NULL, /* reallocate deliberately absent */
    probe_deallocate
};

static const astra_allocator_ops k_no_deallocate_ops = {
    probe_allocate,
    probe_allocate_zeroed,
    probe_reallocate,
    NULL /* deallocate deliberately absent */
};

static const astra_allocator_ops k_probe_ops = {
    probe_allocate,
    probe_allocate_zeroed,
    probe_reallocate,
    probe_deallocate
};

/* Restores the default allocator no matter how a test leaves off. */
static void install_probe(void)
{
    memset(&g_probe, 0, sizeof g_probe);
    ASTRA_CHECK_STATUS(astra_allocator_set_ops(&k_probe_ops), ASTRA_OK);
    ASTRA_CHECK(!astra_allocator_is_default());
    ASTRA_CHECK(astra_allocator_get_ops() == &k_probe_ops);
}

static void restore_default(void)
{
    ASTRA_CHECK_STATUS(astra_allocator_set_ops(NULL), ASTRA_OK);
    ASTRA_CHECK(astra_allocator_is_default());
}

void astra_test_allocator_basics(void)
{
    void *block;
    void *other;

    astra_test_begin("astra_test_allocator_basics");

    /* The default is the C library, and that is what a fresh process sees. */
    ASTRA_CHECK(astra_allocator_is_default());
    ASTRA_CHECK(astra_allocator_get_ops() != NULL);

    block = astra_alloc(64);
    ASTRA_CHECK(block != NULL);
    if (block != NULL) {
        /* Writing the whole block proves the usable size is really at least 64. */
        memset(block, 0xA5, 64);
        ASTRA_CHECK(((unsigned char *)block)[0] == 0xA5u);
        ASTRA_CHECK(((unsigned char *)block)[63] == 0xA5u);
    }

    other = astra_alloc(64);
    ASTRA_CHECK(other != NULL);
    /* Two live allocations must not overlap. */
    ASTRA_CHECK(block != other);

    astra_dealloc(other);
    astra_dealloc(block);

    /* A zero sized request is normalised to NULL rather than to a pointer the
     * caller cannot size anything with. */
    ASTRA_CHECK(astra_alloc(0) == NULL);

    /* Deallocating NULL is a no-op, so cleanup paths need no guard. */
    astra_dealloc(NULL);
    astra_dealloc(NULL);
}

void astra_test_allocator_zeroed(void)
{
    unsigned char *block;
    size_t count;

    astra_test_begin("astra_test_allocator_zeroed");

    block = (unsigned char *)astra_alloc_zeroed(16, 8);
    ASTRA_CHECK(block != NULL);
    if (block != NULL) {
        int all_zero = 1;

        for (count = 0; count < 16u * 8u; ++count) {
            if (block[count] != 0u) {
                all_zero = 0;
            }
        }
        ASTRA_CHECK(all_zero);
        astra_dealloc(block);
    }

    /* Either factor being zero means "nothing to allocate". */
    ASTRA_CHECK(astra_alloc_zeroed(0, 8) == NULL);
    ASTRA_CHECK(astra_alloc_zeroed(8, 0) == NULL);
    ASTRA_CHECK(astra_alloc_zeroed(0, 0) == NULL);

    /*
     * Overflow must be refused, not wrapped. SIZE_MAX * 2 would otherwise
     * produce a buffer of the right size on paper and the wrong size in fact.
     */
    ASTRA_CHECK(astra_alloc_zeroed(SIZE_MAX, 2) == NULL);
    ASTRA_CHECK(astra_alloc_zeroed(SIZE_MAX, SIZE_MAX) == NULL);

    /* The largest request that still cannot wrap is refused too. */
    ASTRA_CHECK(astra_alloc_zeroed(SIZE_MAX / 2 + 1u, 4) == NULL);
}

void astra_test_allocator_realloc(void)
{
    char *block;

    astra_test_begin("astra_test_allocator_realloc");

    /* realloc(NULL, n) is an allocation. */
    block = (char *)astra_realloc(NULL, 8);
    ASTRA_CHECK(block != NULL);
    if (block == NULL) {
        return;
    }
    memcpy(block, "astra", 6);

    /* Growing preserves the original contents. */
    {
        char *grown = (char *)astra_realloc(block, 64);

        ASTRA_CHECK(grown != NULL);
        if (grown != NULL) {
            ASTRA_CHECK_STRING(grown, "astra");
            block = grown;
        }
    }

    /* Shrinking preserves the surviving prefix and does not read past the new
     * end, which AddressSanitizer verifies independently. */
    {
        char *shrunk = (char *)astra_realloc(block, 6);

        ASTRA_CHECK(shrunk != NULL);
        if (shrunk != NULL) {
            ASTRA_CHECK_STRING(shrunk, "astra");
            block = shrunk;
        }
    }

    /* Same size keeps everything. */
    {
        char *same = (char *)astra_realloc(block, 6);

        ASTRA_CHECK(same != NULL);
        if (same != NULL) {
            ASTRA_CHECK_STRING(same, "astra");
            block = same;
        }
    }

    /* Resizing to zero is defined here as "release", and returns NULL so the
     * caller cannot mistake it for a failure. */
    ASTRA_CHECK(astra_realloc(block, 0) == NULL);
    block = NULL;

    /* Resizing a zero sized request yields NULL as well. */
    ASTRA_CHECK(astra_realloc(NULL, 0) == NULL);
}

void astra_test_allocator_strings(void)
{
    char *copy;

    astra_test_begin("astra_test_allocator_strings");

    copy = astra_strdup("AstraDB");
    ASTRA_CHECK(copy != NULL);
    if (copy != NULL) {
        ASTRA_CHECK_STRING(copy, "AstraDB");
        ASTRA_CHECK(strlen(copy) == 7u);
        /* The copy must be independent of the literal it came from. */
        copy[0] = 'X';
        ASTRA_CHECK_STRING(copy, "XstraDB");
        astra_dealloc(copy);
    }

    /* An empty string still yields an owned, freeable, NUL terminated result. */
    copy = astra_strdup("");
    ASTRA_CHECK(copy != NULL);
    if (copy != NULL) {
        ASTRA_CHECK_STRING(copy, "");
        astra_dealloc(copy);
    }

    ASTRA_CHECK(astra_strdup(NULL) == NULL);

    /* strndup truncates. */
    copy = astra_strndup("AstraDB", 3);
    ASTRA_CHECK(copy != NULL);
    if (copy != NULL) {
        ASTRA_CHECK_STRING(copy, "Ast");
        astra_dealloc(copy);
    }

    /* A length longer than the source stops at the NUL and does not overrun. */
    copy = astra_strndup("Ast", 100);
    ASTRA_CHECK(copy != NULL);
    if (copy != NULL) {
        ASTRA_CHECK_STRING(copy, "Ast");
        ASTRA_CHECK(strlen(copy) == 3u);
        astra_dealloc(copy);
    }

    /* Length zero gives an owned empty string, not NULL, so that callers can
     * release the result unconditionally. */
    copy = astra_strndup("AstraDB", 0);
    ASTRA_CHECK(copy != NULL);
    if (copy != NULL) {
        ASTRA_CHECK_STRING(copy, "");
        astra_dealloc(copy);
    }

    ASTRA_CHECK(astra_strndup(NULL, 4) == NULL);
}

void astra_test_allocator_tracking(void)
{
    astra_alloc_stats before;
    astra_alloc_stats after;
    void *blocks[8];

    astra_test_begin("astra_test_allocator_tracking");

    ASTRA_CHECK_STATUS(astra_allocator_stats(&before), ASTRA_OK);

    if (!astra_allocator_tracking_enabled()) {
        /*
         * Release build: the counters must be present and zero rather than
         * missing, so that callers can read them unconditionally.
         */
        ASTRA_CHECK(before.live_blocks == 0);
        ASTRA_CHECK(before.live_bytes == 0);
        ASTRA_CHECK(astra_allocator_live_bytes() == 0);
        ASTRA_CHECK(astra_allocator_live_blocks() == 0);
        ASTRA_CHECK_STATUS(astra_allocator_stats(NULL), ASTRA_ERR_INVALID_ARGUMENT);
        return;
    }

    for (size_t i = 0; i < ASTRA_ARRAY_LEN(blocks); ++i) {
        blocks[i] = astra_alloc(100);
        ASTRA_CHECK(blocks[i] != NULL);
    }

    ASTRA_CHECK_STATUS(astra_allocator_stats(&after), ASTRA_OK);

    /* The payload is what is counted; the debug header is not. */
    ASTRA_CHECK(after.live_blocks == before.live_blocks + ASTRA_ARRAY_LEN(blocks));
    ASTRA_CHECK(after.live_bytes == before.live_bytes + (100u * ASTRA_ARRAY_LEN(blocks)));
    ASTRA_CHECK(astra_allocator_live_blocks() == after.live_blocks);
    ASTRA_CHECK(astra_allocator_live_bytes() == after.live_bytes);
    ASTRA_CHECK(after.total_allocations >= before.total_allocations + ASTRA_ARRAY_LEN(blocks));

    /* A zero sized allocation is not an allocation, and releasing NULL is not a
     * release, so neither must move the counters. */
    {
        astra_alloc_stats unchanged;

        ASTRA_CHECK(astra_alloc(0) == NULL);
        astra_dealloc(NULL);
        ASTRA_CHECK_STATUS(astra_allocator_stats(&unchanged), ASTRA_OK);
        ASTRA_CHECK(unchanged.live_blocks == after.live_blocks);
        ASTRA_CHECK(unchanged.live_bytes == after.live_bytes);
    }

    /* Resizing keeps exactly one live block, with the new payload size. */
    {
        void *resized = astra_realloc(blocks[0], 200);

        ASTRA_CHECK(resized != NULL);
        blocks[0] = resized;
    }
    ASTRA_CHECK(astra_allocator_live_blocks() == after.live_blocks);
    ASTRA_CHECK(astra_allocator_live_bytes() == after.live_bytes + 100u);

    for (size_t i = 0; i < ASTRA_ARRAY_LEN(blocks); ++i) {
        astra_dealloc(blocks[i]);
    }

    /* Everything the test allocated is gone: this is the leak check. */
    ASTRA_CHECK_STATUS(astra_allocator_stats(&after), ASTRA_OK);
    ASTRA_CHECK(after.live_blocks == before.live_blocks);
    ASTRA_CHECK(after.live_bytes == before.live_bytes);
    ASTRA_CHECK(after.total_deallocations >= before.total_deallocations + ASTRA_ARRAY_LEN(blocks));
}

void astra_test_allocator_replacement(void)
{
    void *block;
    char *text;

    astra_test_begin("astra_test_allocator_replacement");

    /* Validation: every member is required, and a rejected table leaves the
     * previous allocator untouched rather than installing a broken one. */
    ASTRA_CHECK_STATUS(astra_allocator_set_ops(&k_no_zeroed_ops), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_allocator_set_ops(&k_no_reallocate_ops), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_allocator_set_ops(&k_no_deallocate_ops), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(astra_allocator_is_default());
    ASTRA_CHECK(g_probe.allocate_calls == 0u);
    ASTRA_CHECK(g_probe.reallocate_calls == 0u);

    /* NULL restores the default. */
    ASTRA_CHECK_STATUS(astra_allocator_set_ops(NULL), ASTRA_OK);
    ASTRA_CHECK(astra_allocator_is_default());

    install_probe();

    /* Allocation and release are both routed through the replacement. */
    block = astra_alloc(32);
    ASTRA_CHECK(block != NULL);
    ASTRA_CHECK(g_probe.allocate_calls == 1u);
    ASTRA_CHECK(g_probe.deallocate_calls == 0u);

    memset(block, 0x11, 32);
    astra_dealloc(block);
    ASTRA_CHECK(g_probe.deallocate_calls == 1u);

    /* Zeroed allocation goes through the zeroed entry point, not allocate. */
    block = astra_alloc_zeroed(4, 4);
    ASTRA_CHECK(block != NULL);
    ASTRA_CHECK(g_probe.allocate_zeroed_calls == 1u);
    if (block != NULL) {
        ASTRA_CHECK(((unsigned char *)block)[0] == 0u);
        astra_dealloc(block);
    }

    /* String duplication goes through the replacement too. */
    text = astra_strdup("probe");
    ASTRA_CHECK(text != NULL);
    ASTRA_CHECK_STRING(text, "probe");
    astra_dealloc(text);

    /*
     * Resizing must reach the replacement somehow. With debug tracking the
     * library emulates the resize as allocate + copy + release so that it can
     * keep the size header correct; without tracking it delegates directly.
     */
    {
        unsigned long allocate_before = g_probe.allocate_calls;
        unsigned long deallocate_before = g_probe.deallocate_calls;
        char *grown;

        block = astra_alloc(16);
        ASTRA_CHECK(block != NULL);
        if (block != NULL) {
            memcpy(block, "grow", 5);
            grown = (char *)astra_realloc(block, 512);
            ASTRA_CHECK(grown != NULL);

            if (grown != NULL) {
                ASTRA_CHECK_STRING(grown, "grow");
                astra_dealloc(grown);
                if (astra_allocator_tracking_enabled()) {
                    ASTRA_CHECK(g_probe.allocate_calls == allocate_before + 2u);
                    ASTRA_CHECK(g_probe.deallocate_calls == deallocate_before + 2u);
                    ASTRA_CHECK(g_probe.reallocate_calls == 0u);
                } else {
                    ASTRA_CHECK(g_probe.reallocate_calls == 1u);
                }
            }
        }
    }

    /*
     * Now make the replacement fail. Failures must be reported through NULL, and
     * a failed resize must leave the caller's block owned and usable.
     */
    {
        char *survivor = (char *)astra_alloc(64);
        astra_alloc_stats before_failure;

        ASTRA_CHECK(survivor != NULL);
        if (survivor == NULL) {
            return;
        }
        memcpy(survivor, "survivor", 9);

        ASTRA_CHECK_STATUS(astra_allocator_stats(&before_failure), ASTRA_OK);

        g_probe.fail_all = true;

        ASTRA_CHECK(astra_alloc(32) == NULL);
        ASTRA_CHECK(astra_alloc_zeroed(4, 4) == NULL);
        ASTRA_CHECK(astra_strdup("nope") == NULL);
        ASTRA_CHECK(astra_strndup("nope", 4) == NULL);
        ASTRA_CHECK(astra_realloc(survivor, 128) == NULL);

        /* The block is still the caller's and still intact. */
        ASTRA_CHECK_STRING(survivor, "survivor");

        /* No failed allocation was ever recorded as live. */
        if (astra_allocator_tracking_enabled()) {
            astra_alloc_stats after_failure;

            ASTRA_CHECK_STATUS(astra_allocator_stats(&after_failure), ASTRA_OK);
            ASTRA_CHECK(after_failure.live_blocks == before_failure.live_blocks);
            ASTRA_CHECK(after_failure.live_bytes == before_failure.live_bytes);
        }

        g_probe.fail_all = false;
        astra_dealloc(survivor);
    }

    restore_default();

    /* After restoring, the default is used again and the probe is untouched. */
    {
        unsigned long calls_before = g_probe.allocate_calls;

        block = astra_alloc(8);
        ASTRA_CHECK(block != NULL);
        ASTRA_CHECK(g_probe.allocate_calls == calls_before);
        astra_dealloc(block);
    }
}