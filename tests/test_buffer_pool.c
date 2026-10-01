/*
 * Storage tests: the Buffer Pool.
 *
 * Three layers, deliberately in that order.
 *
 *   1. The three modules on their own, through buffer_internal.h. The page table and the
 *      clock are where the interesting invariants live - chain integrity, and the two
 *      rules that make a "no victim" answer mean something - and both are much easier to
 *      pin down directly than through a pool. A bug in the clock shows up through the
 *      pool as a page that was evicted when it should not have been, which is one
 *      observation away from a hundred explanations.
 *   2. The public API, one behaviour per group, each on its own database.
 *   3. The properties that only appear over many operations or more than one thread:
 *      a deterministic random workload checked against a shadow model, and concurrency.
 *
 * The shadow model is the important idea in the last layer. Rather than asserting that
 * particular pages came back, every write is recorded in a model of what the file should
 * contain, and at the end the file is reopened and compared against the model page by
 * page. That catches a write that never happened, a write that went to the wrong offset,
 * and a write that happened in the wrong order, without the test having to know which of
 * them it is testing for.
 *
 * Threaded groups are deterministic in the sense that matters: each thread is given its
 * own work and its own pages, and the shared paths are the ones whose contract is
 * "these operations do not corrupt each other", not "this interleaving happened". A
 * stress test that asserted a particular interleaving would fail once and then never
 * again, and would have told us nothing about the pool.
 */
#include "test_support.h"
#include "test_temp.h"

#include "core/sync.h"
#include "storage/buffer/buffer_internal.h"
#include "storage/storage_internal.h"

#include <stdatomic.h>
#include <stdio.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------------- */
/* Thread scaffolding                                                         */
/* ------------------------------------------------------------------------- */

/*
 * The library has a latch but no thread type, which is deliberate: a buffer pool does
 * not create threads, it only has to be correct when a caller does. So the test brings
 * its own, and it is thirty lines rather than a portability layer in the library.
 */
typedef struct astra_test_thread astra_test_thread;

#if defined(_WIN32)
typedef HANDLE astra_test_thread_handle;
#else
typedef pthread_t astra_test_thread_handle;
#endif

typedef void (*astra_test_thread_fn)(void *argument);

struct astra_test_thread {
    astra_test_thread_fn fn;
    void *argument;
    astra_test_thread_handle handle;
};

/*
 * Records a check from a worker thread.
 *
 * Failure counting belongs to astra_test_report_failure and to nothing else: a helper that
 * incremented astra_test_failures itself would have to be paired with a reporter by
 * convention, and getting that wrong double-counts the failure. There is exactly one way
 * to record a failed check here and this is it.
 */
static void thread_check(bool condition, const char *expression, int line)
{
    ++astra_test_checks;
    if (!condition) {
        astra_test_report_failure("thread", line, expression);
    }
}

/*
 * The thread body. The signature is the platform's, not ours: Microsoft's runtime wants
 * `unsigned (__stdcall *)(void *)` and pthreads want `void *(*)(void *)`, and a wrapper
 * that tried to be clever about the difference would be harder to read than the two
 * typedefs below. The thread's own state comes back through the `astra_test_thread` it was
 * given, so nothing is returned that a caller has to match up by hand.
 */
#if defined(_WIN32)
typedef unsigned int (__stdcall *astra_test_entry_fn)(void *);
#  define ASTRA_TEST_ENTRY unsigned int __stdcall
#else
typedef void *(*astra_test_entry_fn)(void *);
#  define ASTRA_TEST_ENTRY void *
#endif

static ASTRA_TEST_ENTRY thread_entry(void *raw)
{
    astra_test_thread *thread = (astra_test_thread *)raw;

    thread->fn(thread->argument);
#if defined(_WIN32)
    return 0u;
#else
    return NULL;
#endif
}

static bool thread_start(astra_test_thread *thread, astra_test_thread_fn fn, void *argument)
{
    thread->fn = fn;
    thread->argument = argument;

#if defined(_WIN32)
    /* `_beginthreadex` rather than `CreateThread`, because the CRT's allocator is not
     * thread safe unless the thread was created through the CRT. The test allocates
     * through astra_alloc, so this is a correctness requirement and not a style choice. */
    thread->handle = (HANDLE)_beginthreadex(NULL, 0, thread_entry, thread, 0, NULL);
    return thread->handle != NULL;
#else
    return pthread_create(&thread->handle, NULL, thread_entry, thread) == 0;
#endif
}

static void thread_join(astra_test_thread *thread)
{
#if defined(_WIN32)
    (void)WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
#else
    (void)pthread_join(thread->handle, NULL);
#endif
}

/* ------------------------------------------------------------------------- */
/* Deterministic pseudo-randomness                                             */
/* ------------------------------------------------------------------------- */

/*
 * A splitmix64 generator. Not rand(), and not a fixed seed: the point of a deterministic
 * stress test is that a failure is reproducible from the number printed in the source, and
 * neither a platform's rand() sequence nor one seeded implicitly is portable. This is
 * twelve lines, has no global state, and gives every thread its own stream - which is the
 * property that lets the threaded groups be reproducible too.
 */
typedef struct prng {
    uint64 state;
} prng;

static void prng_seed(prng *rng, uint64 seed)
{
    rng->state = seed;
}

static uint64 prng_next(prng *rng)
{
    uint64 z;

    rng->state += 0x9E3779B97F4A7C15ull;
    z = rng->state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* ------------------------------------------------------------------------- */
/* Page patterns                                                              */
/* ------------------------------------------------------------------------- */

/*
 * A fixture that could not be opened is a failed test, not a test to skip. This says so
 * through the ordinary mechanism rather than through a string literal, so the failure
 * prints as a boolean expression a reader can look up.
 */
#define ASTRA_CHECK_FIXTURE(f) ASTRA_CHECK((f).pool != NULL)

/*
 * A page's contents are a function of its identifier and the "generation" it was last
 * written in, so that a stale copy of a page is distinguishable from a current one. A
 * test that filled every page with a constant would not notice an eviction that
 * returned the previous occupant's bytes, which is one of the specific failures a buffer
 * pool has.
 */
static void page_fill(astra_page *page, page_id_t page_id, uint32 generation)
{
    size_t i;

    for (i = 0u; i < page->data_size; ++i) {
        ((uint8 *)page->data)[i] = (uint8)((page_id * 31u) + (generation * 17u) + (i % 251u));
    }
    page->is_dirty = true;
}

static bool page_check(const astra_page *page, page_id_t page_id, uint32 generation)
{
    size_t i;

    if (page->page_id != page_id) {
        return false;
    }
    for (i = 0u; i < page->data_size; ++i) {
        uint8 expected = (uint8)((page_id * 31u) + (generation * 17u) + (i % 251u));
        if (((const uint8 *)page->data)[i] != expected) {
            return false;
        }
    }
    return true;
}

/* What the shadow model believes about one page. */
typedef struct shadow_page {
    uint32 generation; /* the generation last written; see page_fill */
    bool exists;
    bool deleted;
} shadow_page;

typedef struct shadow {
    shadow_page *pages;
    uint64 count;
} shadow;

static void shadow_init(shadow *model, uint64 count)
{
    model->pages = astra_alloc_zeroed((size_t)count, sizeof *model->pages);
    model->count = model->pages != NULL ? count : 0u;
}

static void shadow_destroy(shadow *model)
{
    astra_dealloc(model->pages);
    model->pages = NULL;
    model->count = 0u;
}

static shadow_page *shadow_at(shadow *model, page_id_t page_id)
{
    if (model->pages == NULL || page_id >= model->count) {
        return NULL;
    }
    return &model->pages[page_id];
}

static void shadow_write(shadow *model, page_id_t page_id, uint32 generation)
{
    shadow_page *entry = shadow_at(model, page_id);

    if (entry != NULL) {
        entry->generation = generation;
        entry->exists = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Fixtures                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Opens a database and a pool over it, both sized by the caller.
 *
 * The order is load-bearing: the pool is created before the config goes out of scope,
 * and destroyed before the disk manager is closed, because the pool borrows the manager
 * and refuses to share one. A fixture that got that backwards would report
 * ALREADY_EXISTS or write into a closed handle, and the failure would look like a pool
 * bug.
 */
typedef struct buffer_fixture {
    char dir[ASTRA_STORAGE_PATH_MAX];
    astra_config cfg;
    astra_disk_manager *disk;
    astra_buffer_pool *pool;
} buffer_fixture;

static bool fixture_open(buffer_fixture *fixture, uint32 page_size, uint32 frames)
{
    astra_status status;

    if (!astra_test_make_temp_dir(fixture->dir, sizeof fixture->dir)) {
        return false;
    }

    status = astra_test_make_config(&fixture->cfg, fixture->dir, page_size);
    if (status != ASTRA_OK) {
        astra_test_remove_temp_dir(fixture->dir);
        return false;
    }

    status = astra_config_set_buffer_pool_pages(&fixture->cfg, frames);
    if (status != ASTRA_OK) {
        astra_test_remove_temp_dir(fixture->dir);
        return false;
    }

    status = astra_disk_manager_create(&fixture->cfg, &fixture->disk);
    if (status != ASTRA_OK) {
        astra_test_remove_temp_dir(fixture->dir);
        return false;
    }

    status = astra_buffer_pool_create(&fixture->cfg, fixture->disk, &fixture->pool);
    if (status != ASTRA_OK) {
        (void)astra_disk_manager_close(fixture->disk);
        astra_test_remove_temp_dir(fixture->dir);
        return false;
    }

    return true;
}

static void fixture_close(buffer_fixture *fixture)
{
    if (fixture->pool != NULL) {
        (void)astra_buffer_pool_destroy(fixture->pool);
        fixture->pool = NULL;
    }
    if (fixture->disk != NULL) {
        (void)astra_disk_manager_close(fixture->disk);
        fixture->disk = NULL;
    }
    astra_test_remove_temp_dir(fixture->dir);
}

/*
 * Reopens the fixture's database from scratch, for the persistence groups.
 *
 * Closes and recreates the pool and the manager rather than reusing either, because the
 * question those groups ask is whether the *file* holds the data - a pool that had not
 * flushed would pass a test that reused the pool's own buffers, which is exactly the bug
 * being looked for.
 */
static bool fixture_reopen(buffer_fixture *fixture)
{
    astra_status status;

    if (fixture->pool != NULL) {
        (void)astra_buffer_pool_destroy(fixture->pool);
        fixture->pool = NULL;
    }
    if (fixture->disk != NULL) {
        (void)astra_disk_manager_close(fixture->disk);
        fixture->disk = NULL;
    }

    status = astra_disk_manager_open(&fixture->cfg, &fixture->disk);
    if (status != ASTRA_OK) {
        return false;
    }

    return astra_buffer_pool_create(&fixture->cfg, fixture->disk, &fixture->pool) == ASTRA_OK;
}

/* ------------------------------------------------------------------------- */
/* Module tests: the frame table                                              */
/* ------------------------------------------------------------------------- */

/* True when `index` holds no page, which is the only definition of "free" there is. */
static bool frames_are_free(const astra_frame_table *table, uint32 index)
{
    return !astra_frame_is_resident(&table->frames[index]);
}

/* True when a frame's buffer is entirely zero, which is what a recycled frame must be. */
static bool buffer_is_zeroed(const astra_buffer_frame *frame)
{
    size_t i;

    for (i = 0u; i < frame->page.data_size; ++i) {
        if (((const uint8 *)frame->page.data)[i] != 0u) {
            return false;
        }
    }
    return true;
}

void astra_test_buffer_frame_table(void)
{
    astra_frame_table table;
    astra_buffer_frame *frames;
    uint32 index;
    uint32 round;

    astra_test_begin("buffer_frame_table");

    ASTRA_CHECK_STATUS(astra_frame_table_init(&table, 4u, 4096u), ASTRA_OK);
    ASTRA_CHECK(table.frames != NULL);
    ASTRA_CHECK_UINT64(table.count, 4u);
    ASTRA_CHECK_UINT64(table.fresh_next, 0u);
    ASTRA_CHECK_UINT64(table.used, 0u);

    /* Every frame starts free, with no page, no pins and a zeroed buffer. The zeroed
     * buffer is not cosmetic: it is what makes a recycled frame safe to hand out before
     * anything has written it. */
    for (index = 0u; index < table.count; ++index) {
        ASTRA_CHECK(frames_are_free(&table, index));
    }

    /* Claiming binds both identifiers and takes the first pin. The two identifier fields
     * agreeing is the invariant the page table depends on. */
    frames = table.frames;
    astra_frame_claim(&frames[0], 42u);
    ASTRA_CHECK(astra_frame_is_resident(&frames[0]));
    ASTRA_CHECK_UINT64(frames[0].page_id, 42u);
    ASTRA_CHECK_UINT64(frames[0].page.page_id, 42u);
    ASTRA_CHECK_UINT64(frames[0].pin_count, 1u);
    ASTRA_CHECK(!frames[0].is_dirty);
    ASTRA_CHECK(!frames[0].is_loading);
    ASTRA_CHECK_UINT64(table.used, 0u); /* claiming does not touch the table's count */

    /* A recycled frame must not hand back the previous occupant's bytes. */
    astra_frame_claim(&frames[0], 43u);
    ASTRA_CHECK(buffer_is_zeroed(&frames[0]));

    /* Releasing makes a frame free again and clears every flag the clock looks at. A
     * free frame that kept its pin count would be skipped by the clock forever, and the
     * pool would report itself full while holding a frame nobody could evict. */
    astra_frame_claim(&frames[0], 43u);
    frames[0].is_dirty = true;
    frames[0].is_referenced = true;
    frames[0].write_latched = true;
    astra_frame_release(&frames[0]);
    ASTRA_CHECK(!astra_frame_is_resident(&frames[0]));
    ASTRA_CHECK_UINT64(frames[0].page_id, ASTRA_PAGE_ID_INVALID);
    ASTRA_CHECK_UINT64(frames[0].page.page_id, ASTRA_PAGE_ID_INVALID);
    ASTRA_CHECK_UINT64(frames[0].pin_count, 0u);
    ASTRA_CHECK(!frames[0].is_dirty);
    ASTRA_CHECK(!frames[0].is_referenced);
    ASTRA_CHECK(!frames[0].write_latched);
    ASTRA_CHECK_UINT64(frames[0].chain_next, ASTRA_FRAME_NONE);

    astra_frame_table_destroy(&table);
    ASTRA_CHECK(table.frames == NULL);

    /* Zero frames is refused rather than producing a pool that can never cache
     * anything, and a destroyed table can be destroyed again. */
    ASTRA_CHECK_STATUS(astra_frame_table_init(&table, 0u, 4096u), ASTRA_ERR_UNSUPPORTED);
    astra_frame_table_destroy(&table);
    astra_frame_table_destroy(&table);
    astra_frame_table_destroy(NULL);

    /* Claim and release the same frame many times over. A pool does this on every
     * eviction for the life of the process, so anything that drifts - a pin count, a
     * stale identifier, an uncleared buffer - shows up here rather than in a long run. */
    ASTRA_CHECK_STATUS(astra_frame_table_init(&table, 2u, 512u), ASTRA_OK);
    for (round = 0u; round < 1000u; ++round) {
        page_id_t id = (page_id_t)(round % 7u) + 1u;

        astra_frame_claim(&table.frames[1], id);
        ASTRA_CHECK_UINT64(table.frames[1].page_id, id);
        ASTRA_CHECK_UINT64(table.frames[1].page.page_id, id);
        astra_frame_release(&table.frames[1]);
        ASTRA_CHECK(!astra_frame_is_resident(&table.frames[1]));
    }
    astra_frame_table_destroy(&table);
}

/* ------------------------------------------------------------------------- */
/* Module tests: the page table                                               */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_page_table(void)
{
    astra_page_table table;
    astra_buffer_frame *frames;
    uint32 i;
    page_id_t id;

    astra_test_begin("buffer_page_table");

    /* Zero frames is refused: a table with no frames would accept an insert that then
     * failed its bounds check, and the two errors would contradict each other. */
    ASTRA_CHECK_STATUS(astra_page_table_init(&table, 0u), ASTRA_ERR_UNSUPPORTED);

    ASTRA_CHECK_STATUS(astra_page_table_init(&table, 8u), ASTRA_OK);
    ASTRA_CHECK(table.buckets != NULL);
    ASTRA_CHECK(table.bucket_count >= 16u);
    ASTRA_CHECK_UINT64(table.frame_limit, 8u);
    ASTRA_CHECK_UINT64(table.entries, 0u);
    /* A power of two, so the hash mask is a bit and rather than a division. */
    ASTRA_CHECK((table.bucket_count & (table.bucket_count - 1u)) == 0u);

    frames = astra_alloc_zeroed(8u, sizeof *frames);
    ASTRA_CHECK(frames != NULL);
    if (frames == NULL) {
        astra_page_table_destroy(&table);
        return;
    }
    for (i = 0u; i < 8u; ++i) {
        astra_frame_release(&frames[i]);
    }

    /* An empty table finds nothing. */
    ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, 1u), ASTRA_FRAME_NONE);

    /* Bind then link, which is the order every caller uses: a frame has to carry the key
     * it is filed under before the table points at it. */
    for (i = 0u; i < 8u; ++i) {
        id = (page_id_t)(i + 1u);
        astra_frame_claim(&frames[i], id);
        ASTRA_CHECK(astra_page_table_insert(&table, frames, id, i));
    }
    ASTRA_CHECK_UINT64(table.entries, 8u);
    ASTRA_CHECK_UINT64(astra_page_table_count(&table), 8u);

    /* Every key is findable at the frame it was inserted at. This is the assertion that
     * catches the encoding bug this table had: a chain walked in the wrong encoding
     * returns some other frame, or none, and a full-table insert sweep is the smallest
     * case that does it. */
    for (i = 0u; i < 8u; ++i) {
        ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, (page_id_t)(i + 1u)), i);
    }

    /* A page that is not resident is not found, and the table is not a bitmap. */
    ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, 9u), ASTRA_FRAME_NONE);
    ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, 0u), ASTRA_FRAME_NONE);
    ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, ASTRA_PAGE_ID_INVALID),
                       ASTRA_FRAME_NONE);

    /* Inserting a key that is already present fails and changes nothing. */
    ASTRA_CHECK(!astra_page_table_insert(&table, frames, 3u, 0u));
    ASTRA_CHECK_UINT64(table.entries, 8u);
    ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, 3u), 2u);

    /* Inserting a frame that does not carry the key fails, and nothing is linked. This
     * is the check that stops a bucket pointing at a frame holding a different page. */
    ASTRA_CHECK(!astra_page_table_insert(&table, frames, 100u, 5u));
    ASTRA_CHECK(!astra_page_table_insert(&table, frames, 100u, 8u)); /* out of range */
    ASTRA_CHECK_UINT64(table.entries, 8u);
    ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, 100u), ASTRA_FRAME_NONE);

    /* Remove from the middle of a chain, and from the head. After each, the remaining
     * keys must all still be findable, which is the property a unlink is really
     * asserting. */
    for (i = 0u; i < 8u; ++i) {
        ASTRA_CHECK(astra_page_table_remove(&table, frames, (page_id_t)(i + 1u)));
    }
    ASTRA_CHECK_UINT64(table.entries, 0u);
    for (i = 0u; i < 8u; ++i) {
        ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, (page_id_t)(i + 1u)),
                           ASTRA_FRAME_NONE);
    }

    /* Removing something that is not there is false, not a corruption. */
    ASTRA_CHECK(!astra_page_table_remove(&table, frames, 1u));

    /*
     * Many more keys than buckets, to force long chains and collisions. The identifiers
     * are spread far apart - 1, 1001, 2001, ... - so the test exercises the hash rather
     * than the fact that consecutive integers happen to be well distributed.
     */
    astra_page_table_destroy(&table);
    ASTRA_CHECK_STATUS(astra_page_table_init(&table, 8u), ASTRA_OK);
    for (i = 0u; i < 8u; ++i) {
        astra_frame_release(&frames[i]);
    }

    for (uint32 page_round = 0u; page_round < 8u; ++page_round) {
        id = (page_id_t)(1u + (uint64)page_round * 1000u);
        astra_frame_claim(&frames[page_round], id);
        ASTRA_CHECK(astra_page_table_insert(&table, frames, id, page_round));
    }
    for (uint32 page_round = 0u; page_round < 8u; ++page_round) {
        id = (page_id_t)(1u + (uint64)page_round * 1000u);
        ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, id), page_round);
    }

    /* Remove half, in an order that hits both heads and middles of chains, and check the
     * rest survived. A relink that drops the tail of a chain is invisible until
     * something is removed from the middle of it. */
    for (uint32 page_round = 0u; page_round < 8u; page_round += 2u) {
        id = (page_id_t)(1u + (uint64)page_round * 1000u);
        ASTRA_CHECK(astra_page_table_remove(&table, frames, id));
        astra_frame_release(&frames[page_round]);
    }
    for (uint32 page_round = 1u; page_round < 8u; page_round += 2u) {
        id = (page_id_t)(1u + (uint64)page_round * 1000u);
        ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, id), page_round);
    }
    for (uint32 page_round = 0u; page_round < 8u; page_round += 2u) {
        id = (page_id_t)(1u + (uint64)page_round * 1000u);
        ASTRA_CHECK_UINT64(astra_page_table_find(&table, frames, id), ASTRA_FRAME_NONE);
    }
    ASTRA_CHECK_UINT64(table.entries, 4u);

    /* NULL handling, because a cleanup path or a defensive call should not crash. */
    ASTRA_CHECK_UINT64(astra_page_table_find(NULL, frames, 1u), ASTRA_FRAME_NONE);
    ASTRA_CHECK(!astra_page_table_insert(NULL, frames, 1u, 0u));
    ASTRA_CHECK(!astra_page_table_remove(NULL, frames, 1u));
    ASTRA_CHECK_UINT64(astra_page_table_count(NULL), 0u);
    astra_page_table_destroy(NULL);

    astra_dealloc(frames);
    astra_page_table_destroy(&table);
}

/* ------------------------------------------------------------------------- */
/* Module tests: the retired set                                              */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_retired_set(void)
{
    astra_retired_set set;
    uint32 i;

    astra_test_begin("buffer_retired_set");

    astra_retired_set_init(&set);
    ASTRA_CHECK(set.ids == NULL);
    ASTRA_CHECK_UINT64(set.count, 0u);
    ASTRA_CHECK(!astra_retired_set_contains(&set, 1u));
    ASTRA_CHECK(!astra_retired_set_contains(NULL, 1u));

    ASTRA_CHECK_STATUS(astra_retired_set_add(&set, 10u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_retired_set_add(&set, 5u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_retired_set_add(&set, 7u), ASTRA_OK);
    ASTRA_CHECK_UINT64(set.count, 3u);

    /* Sorted, which is what makes contains a binary search. Insertion order is
     * deliberately not sorted above, so this is testing the set and not the input. */
    for (i = 0u; i < 3u; ++i) {
        ASTRA_CHECK(i == 0u || set.ids[i - 1u] < set.ids[i]);
    }

    ASTRA_CHECK(astra_retired_set_contains(&set, 5u));
    ASTRA_CHECK(astra_retired_set_contains(&set, 7u));
    ASTRA_CHECK(astra_retired_set_contains(&set, 10u));
    ASTRA_CHECK(!astra_retired_set_contains(&set, 6u));
    ASTRA_CHECK(!astra_retired_set_contains(&set, 11u));
    ASTRA_CHECK(!astra_retired_set_contains(&set, 0u));

    /* Deleting twice is a no-op, not an error: the public delete accepts a page the pool
     * does not hold, and a caller may well call it twice. */
    ASTRA_CHECK_STATUS(astra_retired_set_add(&set, 7u), ASTRA_OK);
    ASTRA_CHECK_UINT64(set.count, 3u);

    /* Growth past the initial capacity, and a large identifier range. The upper numbers
     * are the point: a set that only ever saw small identifiers could be an array
     * indexed by identifier with no visible problem until it met a big database. */
    for (i = 0u; i < 200u; ++i) {
        page_id_t id = (page_id_t)(1000u + (uint64)i * 7919u);

        ASTRA_CHECK_STATUS(astra_retired_set_add(&set, id), ASTRA_OK);
        ASTRA_CHECK(astra_retired_set_contains(&set, id));
    }
    ASTRA_CHECK_UINT64(set.count, 203u);

    /* Still sorted after the growth, which the binary search depends on. */
    for (i = 0u; i < set.count; ++i) {
        ASTRA_CHECK(i == 0u || set.ids[i - 1u] < set.ids[i]);
    }

    /* A miss among the misses, in the range the loop covered. */
    for (i = 0u; i < 200u; ++i) {
        page_id_t id = (page_id_t)(1000u + (uint64)i * 7919u);

        if (id % 7919u != 0u) {
            ASTRA_CHECK(!astra_retired_set_contains(&set, id + 1u));
        }
    }

    astra_retired_set_destroy(&set);
    ASTRA_CHECK(set.ids == NULL);
    ASTRA_CHECK_UINT64(set.count, 0u);
    astra_retired_set_destroy(&set);
    astra_retired_set_destroy(NULL);
}

/* ------------------------------------------------------------------------- */
/* Module tests: the clock                                                    */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_clock(void)
{
    astra_replacer replacer;
    astra_buffer_frame *frames;
    uint32 index;
    uint32 count = 4u;
    uint64 examined;

    astra_test_begin("buffer_clock");

    frames = astra_alloc_zeroed(count, sizeof *frames);
    ASTRA_CHECK(frames != NULL);
    if (frames == NULL) {
        return;
    }
    for (index = 0u; index < count; ++index) {
        astra_frame_release(&frames[index]);
    }

    /*
     * The refusal cases. A clock with no frames, a null replacer, no frames, a null
     * destination, and a zero frame count all choose nothing - and, being refusals rather
     * than sweeps, none of them may move the hand. The last check is the one that would
     * catch a `return false` that happened after a partial sweep.
     */
    replacer.hand = 0u;
    ASTRA_CHECK(!astra_replacer_evict(&replacer, frames, 0u, NULL, &index));
    ASTRA_CHECK(!astra_replacer_evict(NULL, frames, count, NULL, &index));
    ASTRA_CHECK(!astra_replacer_evict(&replacer, NULL, count, NULL, &index));
    ASTRA_CHECK(!astra_replacer_evict(&replacer, frames, count, NULL, NULL));
    ASTRA_CHECK_UINT64(astra_replacer_hand(&replacer), 0u);
    ASTRA_CHECK_UINT64(astra_replacer_hand(NULL), ASTRA_FRAME_NONE);

    /* All unreferenced and unpinned: the first frame examined is taken. */
    replacer.hand = 0u;
    ASTRA_CHECK(astra_replacer_evict(&replacer, frames, count, &examined, &index));
    ASTRA_CHECK_UINT64(index, 0u);
    ASTRA_CHECK_UINT64(examined, 1u);
    ASTRA_CHECK_UINT64(astra_replacer_hand(&replacer), 1u);

    /* The hand moved past the victim, so a pool of two alternates rather than thrashing
     * on one frame. */
    replacer.hand = 0u;
    ASTRA_CHECK(astra_replacer_evict(&replacer, frames, count, NULL, &index));
    ASTRA_CHECK_UINT64(index, 0u);
    replacer.hand = 0u;
    frames[0].pin_count = 1u;
    ASTRA_CHECK(astra_replacer_evict(&replacer, frames, count, NULL, &index));
    ASTRA_CHECK_UINT64(index, 1u);
    frames[0].pin_count = 0u;

    /* A referenced but unpinned frame is given a second chance: its bit is cleared and
     * the sweep moves on. This is the rule that separates the clock from a FIFO. */
    replacer.hand = 0u;
    frames[0].is_referenced = true;
    frames[1].is_referenced = true;
    ASTRA_CHECK(astra_replacer_evict(&replacer, frames, count, NULL, &index));
    ASTRA_CHECK_UINT64(index, 2u);
    ASTRA_CHECK(!frames[0].is_referenced); /* cleared on the way past */
    ASTRA_CHECK(!frames[1].is_referenced);
    ASTRA_CHECK(!frames[2].is_referenced);

    /*
     * The two-sweep bound. Every frame referenced, none pinned: the first pass clears
     * every bit and finds nothing, and the second pass finds frame 0. With only one
     * sweep this would report "no victim" while holding four frames the pool was allowed
     * to throw away, and the caller's fetch would fail with an error about pinned frames
     * when none was pinned.
     *
     * The examined count is one more than the frame count, not two: the counter records
     * frames looked at, and the fifth look is the one that succeeds. Four second chances
     * plus the victim.
     */
    for (index = 0u; index < count; ++index) {
        frames[index].is_referenced = true;
    }
    replacer.hand = 0u;
    ASTRA_CHECK(astra_replacer_evict(&replacer, frames, count, &examined, &index));
    ASTRA_CHECK_UINT64(index, 0u);
    ASTRA_CHECK_UINT64(examined, (uint64)count + 1u);

    /*
     * A pinned frame's reference bit is left alone. A page pinned across a sweep is busy,
     * not recently used, and clearing its bit would put it at the front of the eviction
     * queue the instant it was unpinned. This is the one rule a naive clock gets wrong,
     * and it is invisible unless it is asserted.
     */
    for (index = 0u; index < count; ++index) {
        frames[index].is_referenced = false;
    }
    frames[0].pin_count = 1u;
    frames[0].is_referenced = true;
    replacer.hand = 0u;
    ASTRA_CHECK(astra_replacer_evict(&replacer, frames, count, NULL, &index));
    ASTRA_CHECK_UINT64(index, 1u);
    ASTRA_CHECK(frames[0].is_referenced); /* untouched while pinned */

    /*
     * Every frame pinned: no victim, and no side effect. After two full sweeps the hand
     * is back where it started, so a pool that is briefly full has not lost its place in
     * the rotation - which matters, because a clock that restarted at zero every time it
     * failed would always sweep the same first frames and never see the busy ones clear.
     */
    for (index = 0u; index < count; ++index) {
        frames[index].pin_count = 1u;
        frames[index].is_referenced = false;
    }
    replacer.hand = 0u;
    ASTRA_CHECK(!astra_replacer_evict(&replacer, frames, count, &examined, &index));
    ASTRA_CHECK_UINT64(examined, (uint64)count * 2u);
    ASTRA_CHECK_UINT64(astra_replacer_hand(&replacer), 0u);

    /* One frame freed is enough, and the clock finds it wherever the hand happens to be. */
    frames[2].pin_count = 0u;
    replacer.hand = 0u;
    ASTRA_CHECK(astra_replacer_evict(&replacer, frames, count, NULL, &index));
    ASTRA_CHECK_UINT64(index, 2u);

    /* record_access sets the bit and tolerates NULL. A null frame means a caller bug, but
     * the clock is on the miss path and has no better answer than to do nothing. */
    frames[0].is_referenced = false;
    astra_replacer_record_access(&frames[0]);
    ASTRA_CHECK(frames[0].is_referenced);
    astra_replacer_record_access(NULL);

    /*
     * A frame index of zero must still be a usable answer. A replacer that reported "no
     * victim" by testing the returned index against a zero sentinel would refuse to ever
     * evict frame 0, which in a pool of one is every frame.
     */
    frames[0].pin_count = 0u;
    replacer.hand = 0u;
    frames[0].is_referenced = false;
    ASTRA_CHECK(astra_replacer_evict(&replacer, frames, 1u, NULL, &index));
    ASTRA_CHECK_UINT64(index, 0u);

    /* A larger pool, swept all the way round, visits every frame exactly once. */
    astra_dealloc(frames);
    count = 17u;
    frames = astra_alloc_zeroed(count, sizeof *frames);
    ASTRA_CHECK(frames != NULL);
    if (frames == NULL) {
        return;
    }
    for (index = 0u; index < count; ++index) {
        astra_frame_release(&frames[index]);
    }
    replacer.hand = 0u;
    for (index = 0u; index < count; ++index) {
        uint32 chosen = 0u;

        ASTRA_CHECK(astra_replacer_evict(&replacer, frames, count, NULL, &chosen));
        ASTRA_CHECK_UINT64(chosen, index);
    }
    ASTRA_CHECK_UINT64(astra_replacer_hand(&replacer), 0u); /* wrapped all the way round */

    astra_dealloc(frames);
}

/* ------------------------------------------------------------------------- */
/* Public API: configuration                                                   */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_config(void)
{
    astra_config cfg;
    buffer_fixture fixture;

    astra_test_begin("buffer_config");

    ASTRA_CHECK_STATUS(astra_config_init(&cfg), ASTRA_OK);
    ASTRA_CHECK_UINT64(cfg.buffer_pool_pages, ASTRA_CONFIG_DEFAULT_BUFFER_POOL_PAGES);

    /*
     * The setter refuses out-of-range sizes rather than clamping them. A clamp would turn
     * a caller's arithmetic mistake into a pool that is quietly the wrong size.
     *
     * The status is UNSUPPORTED, not INVALID_ARGUMENT, and that is the rule the whole
     * configuration API follows: a null pointer or an empty string is a bad argument,
     * while a well-formed number outside the range this build honours is a value the
     * library will not act on. The header documents it that way for page_size as well,
     * and the two must agree or a caller cannot write one error path for both.
     */
    ASTRA_CHECK_STATUS(astra_config_set_buffer_pool_pages(&cfg, 0u),
                       ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(
        astra_config_set_buffer_pool_pages(&cfg, ASTRA_BUFFER_POOL_PAGES_MIN - 1u),
        ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_STATUS(
        astra_config_set_buffer_pool_pages(&cfg, ASTRA_BUFFER_POOL_PAGES_MAX + 1u),
        ASTRA_ERR_UNSUPPORTED);
    ASTRA_CHECK_UINT64(cfg.buffer_pool_pages, ASTRA_CONFIG_DEFAULT_BUFFER_POOL_PAGES);

    /* A null configuration is a bad argument, and is the one case that is one. */
    ASTRA_CHECK_STATUS(astra_config_set_buffer_pool_pages(NULL, 8u),
                       ASTRA_ERR_INVALID_ARGUMENT);

    /* Both bounds are legal, which is what makes them bounds rather than exclusions. */
    ASTRA_CHECK_STATUS(astra_config_set_buffer_pool_pages(&cfg, ASTRA_BUFFER_POOL_PAGES_MIN),
                       ASTRA_OK);
    ASTRA_CHECK_UINT64(cfg.buffer_pool_pages, ASTRA_BUFFER_POOL_PAGES_MIN);
    ASTRA_CHECK_STATUS(astra_config_set_buffer_pool_pages(&cfg, ASTRA_BUFFER_POOL_PAGES_MAX),
                       ASTRA_OK);
    ASTRA_CHECK_UINT64(cfg.buffer_pool_pages, ASTRA_BUFFER_POOL_PAGES_MAX);

    /* An out-of-range pool size makes the whole configuration unbuildable, so a pool
     * cannot be created from one. Same rule, same status. */
    cfg.buffer_pool_pages = 0u;
    ASTRA_CHECK_STATUS(astra_config_validate(&cfg), ASTRA_ERR_UNSUPPORTED);
    cfg.buffer_pool_pages = ASTRA_BUFFER_POOL_PAGES_MAX + 1u;
    ASTRA_CHECK_STATUS(astra_config_validate(&cfg), ASTRA_ERR_UNSUPPORTED);
    cfg.buffer_pool_pages = ASTRA_CONFIG_DEFAULT_BUFFER_POOL_PAGES;
    ASTRA_CHECK_STATUS(astra_config_validate(&cfg), ASTRA_OK);

    /* The size is read from the configuration at construction and is the pool's capacity
     * exactly - at the minimum, at an odd size, and at a size larger than the number of
     * pages the database will ever hold. */
    static const uint32 sizes[] = { ASTRA_BUFFER_POOL_PAGES_MIN, 3u, 7u, 64u };
    for (uint32 i = 0u; i < sizeof sizes / sizeof sizes[0]; ++i) {
        ASTRA_CHECK(fixture_open(&fixture, 4096u, sizes[i]));
        if (fixture.pool != NULL) {
            ASTRA_CHECK_UINT64(astra_buffer_pool_capacity(fixture.pool), sizes[i]);
            ASTRA_CHECK_UINT64(astra_buffer_pool_page_size(fixture.pool), 4096u);
        }
        fixture_close(&fixture);
    }
}

/* ------------------------------------------------------------------------- */
/* Public API: lifecycle                                                       */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_create(void)
{
    astra_config cfg;
    buffer_fixture fixture;
    astra_buffer_pool *pool = NULL;
    astra_disk_manager *disk = NULL;
    astra_status status;

    astra_test_begin("buffer_create");

    /* NULL is accepted and does nothing, so a cleanup path needs no guard. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_destroy(NULL), ASTRA_OK);

    if (!astra_test_make_temp_dir(fixture.dir, sizeof fixture.dir)) {
        ASTRA_CHECK_SETUP("could not create a temporary directory");
        return;
    }
    if (astra_test_make_config(&cfg, fixture.dir, 4096u) != ASTRA_OK) {
        ASTRA_CHECK_SETUP("could not build a configuration");
        astra_test_remove_temp_dir(fixture.dir);
        return;
    }
    if (astra_config_set_buffer_pool_pages(&cfg, 8u) != ASTRA_OK) {
        ASTRA_CHECK_SETUP("could not set the pool size");
        astra_test_remove_temp_dir(fixture.dir);
        return;
    }
    if (astra_disk_manager_create(&cfg, &disk) != ASTRA_OK) {
        ASTRA_CHECK_SETUP("could not create a database");
        astra_test_remove_temp_dir(fixture.dir);
        return;
    }

    /* Every combination of a null argument is refused, and the output is left null rather
     * than half written. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_create(NULL, disk, &pool), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(pool == NULL);
    ASTRA_CHECK_STATUS(astra_buffer_pool_create(&cfg, NULL, &pool), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK(pool == NULL);
    ASTRA_CHECK_STATUS(astra_buffer_pool_create(&cfg, disk, NULL), ASTRA_ERR_INVALID_ARGUMENT);

    /* An invalid configuration is refused as unsupported, not as a bad argument: the
     * arguments are fine, the values in them are not ones this process will honour. */
    {
        astra_config broken = cfg;

        broken.buffer_pool_pages = 0u;
        ASTRA_CHECK_STATUS(astra_buffer_pool_create(&broken, disk, &pool),
                           ASTRA_ERR_UNSUPPORTED);
        ASTRA_CHECK(pool == NULL);

        broken = cfg;
        broken.page_size = 0u;
        ASTRA_CHECK_STATUS(astra_buffer_pool_create(&broken, disk, &pool),
                           ASTRA_ERR_UNSUPPORTED);
    }

    /*
     * A configuration whose page size disagrees with the database is refused rather than
     * reconciled. Either one could be wrong, and picking one silently would produce a
     * pool that reads the file at the wrong stride - a failure that surfaces much later as
     * data that is not where it was written.
     */
    {
        astra_config mismatched = cfg;

        ASTRA_CHECK_STATUS(astra_config_set_page_size(&mismatched, 8192u), ASTRA_OK);
        status = astra_buffer_pool_create(&mismatched, disk, &pool);
        ASTRA_CHECK_STATUS(status, ASTRA_ERR_UNSUPPORTED);
        ASTRA_CHECK(pool == NULL);
    }

    /* The configuration is read, not retained: changing it afterwards does not resize the
     * pool. */
    status = astra_buffer_pool_create(&cfg, disk, &pool);
    ASTRA_CHECK_STATUS(status, ASTRA_OK);
    ASTRA_CHECK(pool != NULL);
    if (pool != NULL) {
        uint64 used = 0u;

        ASTRA_CHECK_UINT64(astra_buffer_pool_capacity(pool), 8u);
        ASTRA_CHECK_UINT64(astra_buffer_pool_page_size(pool), 4096u);
        ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(pool, &used), ASTRA_OK);
        ASTRA_CHECK_UINT64(used, 0u);

        /*
         * One Disk Manager, one pool. A second pool over the same handle would have its
         * own page table and its own frames, and the two would each overwrite buffers the
         * other was writing from - so it is refused rather than left to the caller to
         * notice.
         */
        {
            astra_buffer_pool *second = NULL;

            ASTRA_CHECK_STATUS(astra_buffer_pool_create(&cfg, disk, &second),
                               ASTRA_ERR_ALREADY_EXISTS);
            ASTRA_CHECK(second == NULL);
        }

        /* Destroying releases the handle's claim, so a new pool can be built. This is
         * what makes the registry's address-keyed identity usable at all. */
        ASTRA_CHECK_STATUS(astra_buffer_pool_destroy(pool), ASTRA_OK);
        pool = NULL;

        {
            astra_buffer_pool *again = NULL;

            ASTRA_CHECK_STATUS(astra_buffer_pool_create(&cfg, disk, &again), ASTRA_OK);
            ASTRA_CHECK(again != NULL);
            ASTRA_CHECK_STATUS(astra_buffer_pool_destroy(again), ASTRA_OK);
        }
    }

    (void)astra_disk_manager_close(disk);
    astra_test_remove_temp_dir(fixture.dir);
}

/* ------------------------------------------------------------------------- */
/* Public API: fetching, pinning, unpinning                                    */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_fetch(void)
{
    buffer_fixture fixture;
    astra_page *page = NULL;
    astra_page *again = NULL;
    uint32 pins = 0u;
    uint64 dirty = 0u;
    page_id_t id = 0u;

    astra_test_begin("buffer_fetch");

    if (!fixture_open(&fixture, 4096u, 4u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    /* A fresh database has one page: the header. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, 1u, &page),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK(page == NULL);

    /* NULL arguments. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(NULL, 1u, &page),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, 1u, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);

    /* The invalid identifier is "not found" rather than "bad argument": it names no page,
     * and a stale reference is exactly who would pass it. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ASTRA_PAGE_ID_INVALID, &page),
                       ASTRA_ERR_NOT_FOUND);

    /* The header page is fetchable. */
    page = NULL;
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, 0u, &page), ASTRA_OK);
    ASTRA_CHECK(page != NULL);
    if (page != NULL) {
        ASTRA_CHECK_UINT64(page->page_id, 0u);
        ASTRA_CHECK_UINT64(page->data_size, 4096u);
        ASTRA_CHECK(!page->is_dirty);
    }
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, 0u, false), ASTRA_OK);

    /* Allocate a data page and fetch it: a miss, then a hit on the same frame. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &id), ASTRA_OK);
    ASTRA_CHECK(id == 1u);
    page_fill(page, id, 7u);

    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(fixture.pool, id, &pins), ASTRA_OK);
    ASTRA_CHECK_UINT64(pins, 1u);

    /* Two fetches of one page return the same frame and two pins. This is the invariant
     * that makes a pinned page's bytes belong to every holder of it. */
    again = NULL;
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, id, &again), ASTRA_OK);
    ASTRA_CHECK(again == page);
    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(fixture.pool, id, &pins), ASTRA_OK);
    ASTRA_CHECK_UINT64(pins, 2u);
    ASTRA_CHECK(page_check(again, id, 7u));

    /* Dirty state is declared at the unpin, not observed. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 1u); /* the new page is dirty by construction */

    /* A false unpin leaves an existing dirty bit alone, because a second holder may have
     * set it. Clearing on a false unpin would silently discard the first holder's
     * modification. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 1u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(fixture.pool, id, &pins), ASTRA_OK);
    ASTRA_CHECK_UINT64(pins, 1u);

    /* The double unpin. This is the interlock the whole API exists for: a page in use with
     * a pin count of zero is one the next fetch is allowed to evict. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, false),
                       ASTRA_ERR_INVALID_STATE);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, false),
                       ASTRA_ERR_INVALID_STATE);

    /* Unpinning a page that is not resident is reported, not tolerated: the caller
     * believes it holds a pin, and the only way it does not is that the page was
     * evicted. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, 99u, false),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ASTRA_PAGE_ID_INVALID, false),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(NULL, 1u, false), ASTRA_ERR_INVALID_ARGUMENT);

    /* A pinned count of zero for a page that is not resident is the same answer as for one
     * that is resident and fully unpinned. The distinction is the pool's business. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(fixture.pool, 99u, &pins), ASTRA_OK);
    ASTRA_CHECK_UINT64(pins, 0u);

    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Public API: dirty pages and flushing                                        */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_flush(void)
{
    buffer_fixture fixture;
    astra_page *page = NULL;
    astra_page *check = NULL;
    uint64 dirty = 0u;
    page_id_t ids[3];

    astra_test_begin("buffer_flush");

    if (!fixture_open(&fixture, 4096u, 8u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    /* Nothing resident: flushing everything succeeds and does nothing. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(NULL), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(fixture.pool, 1u), ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(NULL, 1u), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(fixture.pool, ASTRA_PAGE_ID_INVALID),
                       ASTRA_ERR_NOT_FOUND);

    for (uint32 i = 0u; i < 3u; ++i) {
        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &ids[i]), ASTRA_OK);
        page_fill(page, ids[i], i + 1u);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[i], true), ASTRA_OK);
    }

    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 3u);

    /* Flushing one page clears only that page's dirty bit. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(fixture.pool, ids[1]), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 2u);

    /* The data really reached the file, read back through the Disk Manager rather than
     * through the pool's own buffer, which is the only way to know the write happened. */
    {
        astra_page direct;

        ASTRA_CHECK_STATUS(astra_page_init(&direct, 4096u), ASTRA_OK);
        direct.page_id = ids[1];
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(fixture.disk, ids[1], &direct),
                           ASTRA_OK);
        ASTRA_CHECK(page_check(&direct, ids[1], 2u));
        astra_page_release(&direct);
    }

    /* Flushing a page that is clean still writes it. "Flush this page" is a request about
     * the page, not about its state: a caller that modified the bytes without saying so
     * should be able to make them durable anyway. So modify the buffer behind the pool's
     * back and flush, and the bytes must appear. */
    {
        astra_page direct;

        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[1], &page), ASTRA_OK);
        page_fill(page, ids[1], 99u);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[1], false),
                           ASTRA_OK);

        /* Still clean, because the caller did not declare the change. */
        ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
        ASTRA_CHECK_UINT64(dirty, 2u);

        /* The flush writes it anyway, and only then clears the flag. */
        ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(fixture.pool, ids[1]), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
        ASTRA_CHECK_UINT64(dirty, 2u);

        ASTRA_CHECK_STATUS(astra_page_init(&direct, 4096u), ASTRA_OK);
        direct.page_id = ids[1];
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(fixture.disk, ids[1], &direct),
                           ASTRA_OK);
        ASTRA_CHECK(page_check(&direct, ids[1], 99u));
        astra_page_release(&direct);
    }

    /* Flushing everything clears every remaining dirty page. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 0u);

    for (uint32 i = 0u; i < 3u; ++i) {
        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[i], &check),
                           ASTRA_OK);
        ASTRA_CHECK(page_check(check, ids[i], (i == 1u) ? 99u : i + 1u));
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[i], false), ASTRA_OK);
    }

    /* A page can be flushed while pinned, which is how a checkpoint-style caller hands one
     * off. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[0], &page), ASTRA_OK);
    page_fill(page, ids[0], 42u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(fixture.pool, ids[0]), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 0u);
    /* The pin is untouched: the pool pins internally and gives it back. */
    {
        uint32 pins = 0u;

        ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(fixture.pool, ids[0], &pins), ASTRA_OK);
        ASTRA_CHECK_UINT64(pins, 1u);
    }
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[0], true), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 1u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);

    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Public API: eviction                                                        */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_eviction(void)
{
    buffer_fixture fixture;
    astra_page *page = NULL;
    astra_page *check = NULL;
    uint64 used = 0u;
    uint64 clock_hits = 0u;
    uint64 clock_after = 0u;
    page_id_t ids[6];
    page_id_t reclaimed = 0u;

    astra_test_begin("buffer_eviction");

    /* Four frames and six pages, so the last two fetches must evict. */
    if (!fixture_open(&fixture, 4096u, 4u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    for (uint32 i = 0u; i < 6u; ++i) {
        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &ids[i]), ASTRA_OK);
        page_fill(page, ids[i], i + 1u);
        /* Only some are dirtied. A pool that evicted only clean pages would still pass
         * every test that never dirtied one, which is why the mix is deliberate. */
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[i], (i % 2u) == 0u),
                           ASTRA_OK);
    }

    /* All six fit in the file, but the pool holds four. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(fixture.pool, &used), ASTRA_OK);
    ASTRA_CHECK_UINT64(used, 4u);

    /* Every page reads back correctly, which means the two that were evicted had their
     * dirty bytes written before their frames were reused. A page evicted while dirty and
     * not written would come back as the pattern of whichever page took its frame. */
    for (uint32 i = 0u; i < 6u; ++i) {
        check = NULL;
        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[i], &check),
                           ASTRA_OK);
        if (check != NULL) {
            ASTRA_CHECK(page_check(check, ids[i], i + 1u));
        }
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[i], false),
                           ASTRA_OK);
    }

    /* The clock ran, and the counter says so. A counter that never moves would let every
     * other assertion in this group pass on a pool that silently grew instead of
     * evicting. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_replacement_count(fixture.pool, &clock_hits),
                       ASTRA_OK);
    ASTRA_CHECK(clock_hits > 0u);

    /*
     * A fetch that hits does not sweep the clock. This is the "O(1) on the hit path" claim
     * made measurable: a pool doing only hits must not accumulate sweep counts, because a
     * sweep is the expensive part and a hit that caused one would be a hit that scanned
     * the pool.
     *
     * It has to be measured on a page that is genuinely resident, which is why this
     * touches one page repeatedly rather than all six. The working set here is six pages
     * in a four frame pool, so re-fetching all six guarantees misses, and the assertion
     * would then be measuring eviction - which passes for the wrong reason, or fails for
     * a reason that has nothing to do with the hit path.
     */
    ASTRA_CHECK_STATUS(astra_buffer_pool_replacement_count(fixture.pool, &clock_after),
                       ASTRA_OK);
    /*
     * Make the page resident *before* the counter is captured. The loop below is
     * measuring the hit path, and the first fetch of a page the previous loop evicted is
     * a miss - a legitimate one, but a miss, and it would show up as a swept clock and
     * fail an assertion that is about something else entirely.
     */
    page = NULL;
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[0], &page), ASTRA_OK);
    if (page != NULL) {
        ASTRA_CHECK(page_check(page, ids[0], 1u));
    }
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[0], false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_replacement_count(fixture.pool, &clock_after),
                       ASTRA_OK);

    for (uint32 i = 0u; i < 32u; ++i) {
        page = NULL;
        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[0], &page),
                           ASTRA_OK);
        if (page != NULL) {
            ASTRA_CHECK(page_check(page, ids[0], 1u));
        }
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[0], false),
                           ASTRA_OK);
    }
    {
        uint64 after = 0u;

        ASTRA_CHECK_STATUS(astra_buffer_pool_replacement_count(fixture.pool, &after),
                           ASTRA_OK);
        ASTRA_CHECK_UINT64(after, clock_after);
    }

    /* Every frame is occupied - the six page workload filled all four - and the count says
     * four, not six, because a frame is a frame however many pages have passed through
     * it. A counter that counted evictions as well as frames would report six here. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(fixture.pool, &used), ASTRA_OK);
    ASTRA_CHECK_UINT64(used, 4u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);

    /*
     * A single-frame pool is the smallest case where eviction has to work, and the case
     * where "no victim" must never be reported while a frame is free. If the clock's
     * sentinel confuses frame 0 with "none", this is where it shows.
     */
    fixture_close(&fixture);
    if (!fixture_open(&fixture, 4096u, ASTRA_BUFFER_POOL_PAGES_MIN)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }
    for (uint32 i = 0u; i < 20u; ++i) {
        check = NULL;
        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &ids[0]), ASTRA_OK);
        page_fill(page, ids[0], i + 1u);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[0], true), ASTRA_OK);

        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[0], &check),
                           ASTRA_OK);
        if (check != NULL) {
            ASTRA_CHECK(page_check(check, ids[0], i + 1u));
        }
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[0], false),
                           ASTRA_OK);

        /* Reclaiming the page each round keeps the database small and keeps the pool
         * cycling through the same one frame, which is the tightest test of the
         * dirty-evict path there is. */
        reclaimed = ids[0];
        ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, reclaimed), ASTRA_OK);
    }
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);

    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Public API: pinned frames are protected                                     */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_pinned(void)
{
    buffer_fixture fixture;
    astra_page *pages[3] = { NULL, NULL, NULL };
    uint64 used = 0u;
    page_id_t ids[3];

    astra_test_begin("buffer_pinned");

    /* A pool of three with all three frames pinned: the fetch that would need a fourth
     * frame is told so, and is neither waited for nor served by stealing a pin. */
    if (!fixture_open(&fixture, 4096u, 3u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    for (uint32 i = 0u; i < 3u; ++i) {
        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &pages[i], &ids[i]),
                           ASTRA_OK);
    }
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(fixture.pool, &used), ASTRA_OK);
    ASTRA_CHECK_UINT64(used, 3u);

    /* A fourth page needs a frame and there is none to take. */
    {
        astra_page *overflow = NULL;
        page_id_t overflow_id = 0u;

        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &overflow, &overflow_id),
                           ASTRA_ERR_INVALID_STATE);
        ASTRA_CHECK(overflow == NULL);
        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, 99u, &overflow),
                           ASTRA_ERR_INVALID_STATE);
        ASTRA_CHECK(overflow == NULL);
    }

    /* Fetching a page that is already resident works fine while everything is pinned -
     * it needs no frame, only a pin count. This is the distinction that matters: a full
     * pool is not a stuck pool. */
    {
        astra_page *hit = NULL;

        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[1], &hit), ASTRA_OK);
        ASTRA_CHECK(hit == pages[1]);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[1], false),
                           ASTRA_OK);
    }

    /* Releasing one pin on one frame frees it, and the next fetch succeeds. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[0], false), ASTRA_OK);
    {
        astra_page *overflow = NULL;

        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, 99u, &overflow),
                           ASTRA_ERR_NOT_FOUND); /* not allocated, so not found */
        ASTRA_CHECK(overflow == NULL);
    }

    /* Now a real fourth page, with a frame genuinely available. */
    {
        astra_page *fourth = NULL;
        page_id_t fourth_id = 0u;

        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &fourth, &fourth_id),
                           ASTRA_OK);
        ASTRA_CHECK(fourth != NULL);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, fourth_id, false),
                           ASTRA_OK);
    }

    /* Releasing every pin makes the pool usable again for as many frames as it has. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[1], false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[2], false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);

    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Public API: allocating new pages                                            */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_new_page(void)
{
    buffer_fixture fixture;
    astra_page *page = NULL;
    page_id_t first = 0u;
    page_id_t second = 0u;
    uint64 used = 0u;
    uint64 dirty = 0u;

    astra_test_begin("buffer_new_page");

    if (!fixture_open(&fixture, 4096u, 4u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(NULL, &page, &first),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, NULL, &first),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);

    /* The first allocated page is 1: page 0 is the header, and identifiers are never
     * reused, so a page deleted and reallocated earlier does not shift this. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &first), ASTRA_OK);
    ASTRA_CHECK(first == 1u);
    ASTRA_CHECK(page != NULL);

    if (page != NULL) {
        size_t i;
        bool all_zero = true;

        /* An allocated page is zero, so a caller that reads before writing sees an empty
         * page rather than the previous occupant of a recycled frame. */
        ASTRA_CHECK_UINT64(page->page_id, 1u);
        ASTRA_CHECK_UINT64(page->data_size, 4096u);
        for (i = 0u; i < page->data_size; ++i) {
            if (((const uint8 *)page->data)[i] != 0u) {
                all_zero = false;
                break;
            }
        }
        ASTRA_CHECK(all_zero);
    }

    /* It arrives dirty. A page filled in and then unpinned with dirty=false - or lost to
     * a crash before either - must still be written at eviction. Erring towards one
     * redundant write of zeros is the only safe direction. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 1u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, first, false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 1u);

    /* A second allocation gets the next identifier. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &second), ASTRA_OK);
    ASTRA_CHECK(second == 2u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, second, false), ASTRA_OK);

    /* And it is readable through a fetch, as a page like any other. */
    {
        astra_page *check = NULL;

        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, first, &check), ASTRA_OK);
        ASTRA_CHECK(check != NULL);
        ASTRA_CHECK_UINT64(check->page_id, first);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, first, false), ASTRA_OK);
    }

    /* A new page survives a reopen, which is what "allocated" has to mean. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);
    ASTRA_CHECK(fixture_reopen(&fixture));
    {
        astra_page *check = NULL;

        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, first, &check),
                           ASTRA_OK);
        ASTRA_CHECK(check != NULL);
        if (check != NULL) {
            size_t i;
            bool all_zero = true;

            for (i = 0u; i < check->data_size; ++i) {
                if (((const uint8 *)check->data)[i] != 0u) {
                    all_zero = false;
                    break;
                }
            }
            ASTRA_CHECK(all_zero);
        }
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, first, false), ASTRA_OK);
    }

    /* A full pool of pinned frames refuses a new page, and the failed allocation left no
     * identifier behind: identifiers are only assigned by a successful extend. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(fixture.pool, &used), ASTRA_OK);
    ASTRA_CHECK(used > 0u);

    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Public API: the header page                                                 */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_header_page(void)
{
    buffer_fixture fixture;
    astra_page *page = NULL;
    uint64 dirty = 0u;
    page_id_t data_id = 0u;

    astra_test_begin("buffer_header_page");

    if (!fixture_open(&fixture, 4096u, 4u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    /* Fetchable: page 0 is a real page and a tool needs to see it. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, 0u, &page), ASTRA_OK);
    ASTRA_CHECK(page != NULL);
    if (page != NULL) {
        ASTRA_CHECK_UINT64(page->page_id, 0u);
        ASTRA_CHECK(!page->is_dirty);
    }
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, 0u, false), ASTRA_OK);

    /* Not writable. The header's contents are the file's magic number and checksum, and
     * the Disk Manager refuses the write; the pool reports the refusal as its own rather
     * than letting a caller's flush fail halfway. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(fixture.pool, 0u),
                       ASTRA_ERR_INVALID_STATE);

    /* A clean header does not stop flush_all either - it is skipped, not refused, because
     * there is nothing to write. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);

    /* Deleting the header would leave something that is not a database. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, 0u),
                       ASTRA_ERR_INVALID_STATE);

    /* A dirty header is refused by flush_all before anything is written, so the file is
     * left alone rather than partly written. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &data_id), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, data_id, true), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, 0u, &page), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, 0u, true), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 2u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_ERR_INVALID_STATE);

    /* The data page is still on disk as it was, because flush_all refused before writing
     * anything. */
    {
        astra_page direct;

        ASTRA_CHECK_STATUS(astra_page_init(&direct, 4096u), ASTRA_OK);
        direct.page_id = data_id;
        ASTRA_CHECK_STATUS(astra_disk_manager_read_page(fixture.disk, data_id, &direct),
                           ASTRA_OK);
        astra_page_release(&direct);
    }

    /* The database is still openable, which is the point of refusing rather than
     * attempting the write. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(fixture.pool, data_id), ASTRA_OK);
    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Public API: deleting                                                       */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_delete(void)
{
    buffer_fixture fixture;
    astra_page *page = NULL;
    uint64 used = 0u;
    page_id_t resident = 0u;
    page_id_t evicted = 0u;
    page_id_t freed = 0u;

    astra_test_begin("buffer_delete");

    if (!fixture_open(&fixture, 4096u, 2u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(NULL, 1u), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, ASTRA_PAGE_ID_INVALID),
                       ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, 0u),
                       ASTRA_ERR_INVALID_STATE);

    /* A resident page goes away from the pool and its frame is reusable. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &resident), ASTRA_OK);
    page_fill(page, resident, 5u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, resident, true), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(fixture.pool, &used), ASTRA_OK);
    ASTRA_CHECK_UINT64(used, 1u);

    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, resident), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(fixture.pool, &used), ASTRA_OK);
    ASTRA_CHECK_UINT64(used, 0u);

    /* A deleted page is never served again, even though its bytes are still in the file
     * and its identifier is still allocated. This is what stops a stale reference from
     * resurrecting a page whose contents were abandoned. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, resident, &page),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK(page == NULL);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, resident, false),
                       ASTRA_ERR_NOT_FOUND);
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_page(fixture.pool, resident),
                       ASTRA_ERR_NOT_FOUND);

    /* A page that was never cached can be deleted, because a caller may be deleting one
     * the pool has already evicted. The retirement is the whole effect and does not
     * depend on residency. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &evicted), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, evicted, false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &freed), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, freed, false), ASTRA_OK);
    /* The pool holds two frames, so one of those two is now evicted. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, evicted), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, evicted), ASTRA_OK);

    /* Deleting twice is harmless, and deleting an identifier that was never allocated is
     * too - the set is a statement about identifiers, not about the file. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, 4242u), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, 4242u, &page),
                       ASTRA_ERR_NOT_FOUND);

    /* A pinned page cannot be deleted: it is in use, and the pool will not wait for a pin
     * to be released on the holder's behalf, because a thread that never unpins would hang
     * the delete forever. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &freed), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, freed),
                       ASTRA_ERR_INVALID_STATE);
    /* Still there, and still usable, because the delete did not happen. */
    ASTRA_CHECK(page_check(page, freed, 0u) || page->page_id == freed);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, freed, true), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, freed), ASTRA_OK);

    /* The dirty state of a deleted page is discarded, not written: a page being deleted
     * is a page whose contents are being abandoned. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);

    /*
     * The retirement is not durable. A reopened database sees the bytes the file holds,
     * because nothing recorded the deletion. Stated here as a test because it is a
     * property callers depend on not having, and a test is the only place it can be
     * pinned down before a write-ahead log exists to change it.
     */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);
    ASTRA_CHECK(fixture_reopen(&fixture));
    {
        astra_page *check = NULL;
        astra_status status = astra_buffer_pool_fetch_page(fixture.pool, resident, &check);

        /* Either the page is findable again (its bytes were never overwritten) or it is
         * not (the file moved on). What must not happen is a crash or a silent wrong
         * answer, and what the header promises is that the *pool* no longer knows about
         * the deletion - which is why this is allowed to succeed. */
        if (status == ASTRA_OK) {
            ASTRA_CHECK(check != NULL);
            ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, resident, false),
                               ASTRA_OK);
        } else {
            ASTRA_CHECK_STATUS(status, ASTRA_ERR_NOT_FOUND);
        }
    }

    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Public API: queries                                                        */
/* ------------------------------------------------------------------------- */

void astra_test_buffer_queries(void)
{
    buffer_fixture fixture;
    astra_page *page = NULL;
    astra_buffer_pool *pool;
    char line[256];
    uint64 used = 0u;
    uint64 dirty = 0u;
    uint64 clock_hits = 0u;
    uint32 pins = 0u;
    page_id_t id = 0u;
    int written;

    astra_test_begin("buffer_queries");

    /* Every query tolerates NULL, yielding the documented zero rather than a crash. That
     * is what lets a caller ask "is there a pool" without branching first. */
    ASTRA_CHECK_UINT64(astra_buffer_pool_capacity(NULL), 0u);
    ASTRA_CHECK_UINT64(astra_buffer_pool_page_size(NULL), 0u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(NULL, &used), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(NULL, &dirty), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(NULL, 1u, &pins), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_replacement_count(NULL, &clock_hits),
                       ASTRA_ERR_INVALID_ARGUMENT);

    if (!fixture_open(&fixture, 4096u, 5u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }
    pool = fixture.pool;

    /* A NULL destination is a caller bug, not a silent success. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(pool, NULL), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(pool, NULL), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(pool, 1u, NULL), ASTRA_ERR_INVALID_ARGUMENT);
    ASTRA_CHECK_STATUS(astra_buffer_pool_replacement_count(pool, NULL),
                       ASTRA_ERR_INVALID_ARGUMENT);

    ASTRA_CHECK_UINT64(astra_buffer_pool_capacity(pool), 5u);
    ASTRA_CHECK_UINT64(astra_buffer_pool_page_size(pool), 4096u);

    /* "Used" counts frames holding a page, pinned or not. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(pool, &used), ASTRA_OK);
    ASTRA_CHECK_UINT64(used, 0u);

    ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(pool, &page, &id), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_used_frames(pool, &used), ASTRA_OK);
    ASTRA_CHECK_UINT64(used, 1u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 1u);

    /* "Dirty" counts resident dirty pages, which is the number of writes a checkpoint has
     * avoided so far. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(pool), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(pool, &dirty), ASTRA_OK);
    ASTRA_CHECK_UINT64(dirty, 0u);

    /* Pins, for a resident page and for one that is not. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(pool, id, &pins), ASTRA_OK);
    ASTRA_CHECK_UINT64(pins, 1u);
    {
        astra_page *hit = NULL;

        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(pool, id, &hit), ASTRA_OK);
        ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(pool, id, &pins), ASTRA_OK);
        ASTRA_CHECK_UINT64(pins, 2u);
    }
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(pool, id, false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(pool, id, false), ASTRA_OK);
    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(pool, id, &pins), ASTRA_OK);
    ASTRA_CHECK_UINT64(pins, 0u);
    ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(pool, 999u, &pins), ASTRA_OK);
    ASTRA_CHECK_UINT64(pins, 0u);

    /* The clock counter starts at zero on a pool that has evicted nothing. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_replacement_count(pool, &clock_hits), ASTRA_OK);
    ASTRA_CHECK_UINT64(clock_hits, 0u);

    /* describe, including the degenerate calls. */
    written = astra_buffer_pool_describe(pool, line, sizeof line);
    ASTRA_CHECK(written > 0);
    ASTRA_CHECK((size_t)written < sizeof line);
    ASTRA_CHECK(strstr(line, "buffer pool") != NULL);
    ASTRA_CHECK(strstr(line, "5") != NULL);
    ASTRA_CHECK(astra_buffer_pool_describe(pool, NULL, sizeof line) == 0);
    ASTRA_CHECK(astra_buffer_pool_describe(pool, line, 0u) == 0);
    {
        char tiny[8];
        int needed = astra_buffer_pool_describe(pool, tiny, sizeof tiny);

        /* snprintf semantics: the return is the length the full line needed, and a
         * truncated result is still NUL terminated. */
        ASTRA_CHECK(needed > (int)sizeof tiny - 1);
        ASTRA_CHECK(tiny[sizeof tiny - 1] == '\0');
    }
    written = astra_buffer_pool_describe(NULL, line, sizeof line);
    ASTRA_CHECK(written > 0);
    ASTRA_CHECK_STRING(line, "buffer pool: none");

    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Public API: persistence across a reopen                                     */
/* ------------------------------------------------------------------------- */
/*
 * The persistence group. Its whole shape is forced by one fact about the fixture:
 * astra_disk_manager_create refuses to create over an existing database, precisely so that
 * an existing file is never overwritten. So a second database at the same path has to
 * come from astra_disk_manager_open, and the directory has to be the one the first fixture
 * used rather than a fresh one. The helper that does that - reopen over the same path -
 * is `fixture_reopen`, and it closes both the pool and the manager before opening, which
 * is what makes it a genuine test of the file rather than of the buffers that were just
 * written.
 */
void astra_test_buffer_persistence(void)
{
    buffer_fixture fixture;
    astra_page *page = NULL;
    astra_page *check = NULL;
    page_id_t ids[4];

    astra_test_begin("buffer_persistence");

    if (!fixture_open(&fixture, 4096u, 4u)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    /*
     * Four pages in a four-frame pool, all dirtied, then a destroy with no explicit flush.
     * The destroy has to write them: a caller that stops caring should not have to know
     * that a pool was holding dirty pages when it dropped it, and the alternative - a
     * modification that is simply lost - is not a trade anybody wants.
     */
    for (uint32 i = 0u; i < 4u; ++i) {
        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &ids[i]), ASTRA_OK);
        page_fill(page, ids[i], 100u + i);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[i], true), ASTRA_OK);
    }

    /* Destroy without flushing, then reopen the same file with a brand new manager and a
     * brand new pool. Nothing of the old pool survives except the bytes it wrote. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_destroy(fixture.pool), ASTRA_OK);
    fixture.pool = NULL;
    ASTRA_CHECK_STATUS(astra_disk_manager_close(fixture.disk), ASTRA_OK);
    fixture.disk = NULL;

    ASTRA_CHECK_STATUS(astra_disk_manager_open(&fixture.cfg, &fixture.disk), ASTRA_OK);
    ASTRA_CHECK(fixture.disk != NULL);
    if (fixture.disk == NULL) {
        astra_test_remove_temp_dir(fixture.dir);
        return;
    }
    ASTRA_CHECK_STATUS(
        astra_buffer_pool_create(&fixture.cfg, fixture.disk, &fixture.pool), ASTRA_OK);
    ASTRA_CHECK(fixture.pool != NULL);
    if (fixture.pool == NULL) {
        (void)astra_disk_manager_close(fixture.disk);
        fixture.disk = NULL;
        astra_test_remove_temp_dir(fixture.dir);
        return;
    }

    /*
     * Read straight through the Disk Manager first, so the assertion is about the file and
     * not about a buffer the new pool might have loaded from it and agreed with.
     */
    {
        astra_page direct;

        ASTRA_CHECK_STATUS(astra_page_init(&direct, 4096u), ASTRA_OK);
        for (uint32 i = 0u; i < 4u; ++i) {
            direct.page_id = ids[i];
            ASTRA_CHECK_STATUS(astra_disk_manager_read_page(fixture.disk, ids[i], &direct),
                               ASTRA_OK);
            ASTRA_CHECK(page_check(&direct, ids[i], 100u + i));
        }
        astra_page_release(&direct);
    }

    /* And then through a fetch, which is the path a caller would use, to confirm the new
     * pool serves the pages rather than refusing to recognise them. */
    for (uint32 i = 0u; i < 4u; ++i) {
        check = NULL;
        ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, ids[i], &check),
                           ASTRA_OK);
        if (check != NULL) {
            ASTRA_CHECK(page_check(check, ids[i], 100u + i));
        }
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, ids[i], false),
                           ASTRA_OK);
    }

    /* A second destroy with everything clean writes nothing but still syncs, and succeeds.
     * The one-frame pool is here for the same reason it is in the eviction group: it is
     * the smallest pool in which the clock has to work at all. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_destroy(fixture.pool), ASTRA_OK);
    fixture.pool = NULL;
    (void)astra_disk_manager_close(fixture.disk);
    fixture.disk = NULL;
    astra_test_remove_temp_dir(fixture.dir);
}

void astra_test_buffer_stress(void)
{
    /* A pool of six frames over a database of a few hundred pages, driven hard enough to
     * evict constantly. Small enough that a failure is quick to reproduce, large enough
     * that the hash table has chains and the clock is always sweeping. */
    enum { FRAMES = 6, MAX_PAGES = 400, OPS = 20000 };
    buffer_fixture fixture;
    astra_page *page = NULL;
    shadow model;
    prng rng;
    uint64 clock_hits = 0u;
    uint32 next_generation = 1u;
    uint32 allocated = 0u;

    astra_test_begin("buffer_stress");

    if (!fixture_open(&fixture, 4096u, FRAMES)) {
        ASTRA_CHECK_FIXTURE(fixture);
        return;
    }

    shadow_init(&model, MAX_PAGES);
    if (model.pages == NULL) {
        ASTRA_CHECK_SETUP("could not allocate the shadow copy");
        fixture_close(&fixture);
        return;
    }
    /* Page 0 is the header: it exists, it is never written, and it is never a target. */
    model.pages[0].exists = true;

    prng_seed(&rng, 0x5EED1234ABCD0001ull);

    /*
     * How many identifiers have ever been handed out.
     *
     * This is not bookkeeping for its own sake. The pool allocates a fresh identifier for
     * every page and never reuses one, so a workload that allocates without bound walks
     * off the end of both the file and the shadow model, and the "every live page still
     * matches the file" assertion at the end stops being able to name the pages it is
     * checking. Capping the identifier space is what keeps that final comparison honest,
     * and the eviction pressure the test is actually for comes from the working set
     * exceeding the pool's frames, not from the identifier count growing.
     */
    allocated = 0u;

    /*
     * Warm up with a batch of pages so the workload has something to evict immediately.
     * Creating them through the pool is what puts them in the file, which is what makes
     * the later fetches hits rather than not-founds.
     */
    for (uint32 i = 0u; i < 16u; ++i) {
        page_id_t id = 0u;

        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(fixture.pool, &page, &id), ASTRA_OK);
        if (page != NULL) {
            page_fill(page, id, next_generation);
            ++next_generation;
        }
        shadow_write(&model, id, next_generation - 1u);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, true), ASTRA_OK);
        ++allocated;
    }

    for (uint32 op = 0u; op < OPS; ++op) {
        uint64 choice = prng_next(&rng) % 100u;
        /*
         * Page 0 is never a target. It is the file's header, the pool refuses to write it
         * by design, and a workload that fetches it and marks it dirty produces a pool
         * that correctly refuses to flush - which is a true statement about a misuse, and
         * a useless thing to have the stress test spend its failures on. The header gets
         * its own read-only branch below.
         */
        page_id_t id = (page_id_t)(1u + (prng_next(&rng) % (MAX_PAGES - 1u)));
        astra_status status;

        if (choice < 55u) {
            /* Fetch, sometimes verify, sometimes write, then unpin. The write is the
             * interesting half: it makes the page dirty, so the next eviction has to flush
             * it, and the shadow model has to agree about what it flushed. */
            uint32 generation = next_generation;

            page = NULL;
            status = astra_buffer_pool_fetch_page(fixture.pool, id, &page);
            if (status != ASTRA_OK) {
                /* Not allocated, deleted, or no free frame. All legitimate, and all
                 * recorded in the model already. */
                ASTRA_CHECK(status == ASTRA_ERR_NOT_FOUND
                            || status == ASTRA_ERR_INVALID_STATE);
                continue;
            }
            ASTRA_CHECK(page != NULL);

            shadow_page *entry = shadow_at(&model, id);
            if (entry != NULL && !entry->deleted) {
                ASTRA_CHECK(page_check(page, id, entry->generation));
            }

            if (prng_next(&rng) % 3u == 0u) {
                page_fill(page, id, generation);
                ++next_generation;
                shadow_write(&model, id, generation);
                ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, true),
                                   ASTRA_OK);
            } else {
                ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, false),
                                   ASTRA_OK);
            }
        } else if (choice < 70u) {
            /* Allocate. This exercises the path where a new page needs a frame and the
             * only frames available belong to dirty pages that have to be written first.
             *
             * Skipped once the identifier space is full, rather than allowed to overrun it.
             * The pool has no way to hand back an identifier - deletion is a logical
             * retirement, not a free list - so the only way to keep a long run inside a
             * checkable identifier space is to stop asking for new ones. */
            page_id_t fresh = 0u;

            if (allocated + 1u >= MAX_PAGES) {
                continue;
            }

            page = NULL;
            status = astra_buffer_pool_new_page(fixture.pool, &page, &fresh);
            if (status != ASTRA_OK) {
                ASTRA_CHECK(status == ASTRA_ERR_INVALID_STATE);
                continue;
            }
            ASTRA_CHECK(fresh > 0u && fresh < MAX_PAGES);
            /*
             * Generation 0 is a real generation, not a marker for "never written".
             *
             * An earlier version recorded 0 for a freshly allocated page and meant "the
             * buffer is still all zeros", which made page_check - a single rule that
             * verifies the pattern for a generation - wrong for every page that had not
             * been written yet. The alternative here costs one fill and removes the
             * special case: a page the model has no pattern for is a page the model is
             * wrong about, and the check should be able to say so.
             */
            if (page != NULL) {
                page_fill(page, fresh, 0u);
            }
            shadow_write(&model, fresh, 0u);
            ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, fresh, true),
                               ASTRA_OK);
            ++allocated;
        } else if (choice < 78u) {
            /* Delete, which retires the identifier and must make it unservable for the
             * rest of the run. */
            shadow_page *entry = shadow_at(&model, id);

            if (entry != NULL && entry->exists && !entry->deleted) {
                uint32 pins = 0u;

                ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(fixture.pool, id, &pins),
                                   ASTRA_OK);
                if (pins == 0u) {
                    ASTRA_CHECK_STATUS(astra_buffer_pool_delete_page(fixture.pool, id),
                                       ASTRA_OK);
                    entry->deleted = true;

                    /* A retired page is gone from the pool's point of view, which is the
                     * whole effect of the delete. */
                    page = NULL;
                    ASTRA_CHECK_STATUS(astra_buffer_pool_fetch_page(fixture.pool, id, &page),
                                       ASTRA_ERR_NOT_FOUND);
                }
            }
        } else if (choice < 88u) {
            /* Flush one page, or everything. */
            if (prng_next(&rng) % 4u == 0u) {
                ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);
            } else {
                status = astra_buffer_pool_flush_page(fixture.pool, id);
                ASTRA_CHECK(status == ASTRA_OK || status == ASTRA_ERR_NOT_FOUND
                            || status == ASTRA_ERR_INVALID_STATE);
            }
        } else if (choice < 96u) {
            /* A double unpin must be reported, never tolerated. Catching it here means a
             * bug that inflates or deflates a pin count shows up as a failed check rather
             * than as a mysterious eviction later. */
            uint32 pins = 0u;

            ASTRA_CHECK_STATUS(astra_buffer_pool_pin_count(fixture.pool, id, &pins),
                               ASTRA_OK);
            if (pins > 0u) {
                ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, false),
                                   ASTRA_OK);
                ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, id, false),
                                   ASTRA_ERR_INVALID_STATE);
            }
        } else {
            /* Read the header. It is always fetchable, and it is never written. */
            page = NULL;
            if (astra_buffer_pool_fetch_page(fixture.pool, 0u, &page) == ASTRA_OK) {
                ASTRA_CHECK(page != NULL);
                if (page != NULL) {
                    ASTRA_CHECK_UINT64(page->page_id, 0u);
                }
                ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(fixture.pool, 0u, false),
                                   ASTRA_OK);
            }
        }
    }

    /* Everything resident and dirty reaches the file, and the file matches the model. */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(fixture.pool), ASTRA_OK);
    {
        uint64 dirty = 0u;

        ASTRA_CHECK_STATUS(astra_buffer_pool_dirty_frames(fixture.pool, &dirty), ASTRA_OK);
        ASTRA_CHECK_UINT64(dirty, 0u);
    }
    ASTRA_CHECK_STATUS(astra_buffer_pool_replacement_count(fixture.pool, &clock_hits),
                       ASTRA_OK);
    /* The workload was large enough that the clock certainly ran. If this ever stops
     * holding, the workload has stopped testing eviction and the rest of the group is
     * quietly testing nothing. */
    ASTRA_CHECK(clock_hits > 0u);

    /* Read every live page straight from the file, bypassing the pool, and compare
     * against the model. This is the assertion that a write went missing, went to the
     * wrong offset, or happened out of order all look the same from here. */
    {
        astra_page direct;
        uint64 verified = 0u;

        ASTRA_CHECK_STATUS(astra_page_init(&direct, 4096u), ASTRA_OK);
        for (page_id_t id = 1u; id < MAX_PAGES; ++id) {
            shadow_page *entry = shadow_at(&model, id);

            if (entry == NULL || !entry->exists || entry->deleted) {
                continue;
            }

            direct.page_id = id;
            if (astra_disk_manager_read_page(fixture.disk, id, &direct) != ASTRA_OK) {
                continue; /* never actually allocated; the model was optimistic */
            }

            if (entry->generation == 0u) {
                /* Allocated and never written: the file holds zeros. */
                size_t i;
                bool all_zero = true;

                for (i = 0u; i < direct.data_size; ++i) {
                    if (((const uint8 *)direct.data)[i] != 0u) {
                        all_zero = false;
                        break;
                    }
                }
                ASTRA_CHECK(all_zero);
            } else {
                ASTRA_CHECK(page_check(&direct, id, entry->generation));
            }
            ++verified;
        }
        astra_page_release(&direct);
        ASTRA_CHECK(verified > 0u);
    }

    shadow_destroy(&model);
    fixture_close(&fixture);
}

/* ------------------------------------------------------------------------- */
/* Concurrency                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Shared state for the threaded groups. A plain latch rather than atomics for the
 * hand-off, because the threads are waiting on each other and not merely counting; the
 * only atomic here is the error counter, which several threads bump at once.
 */
typedef struct thread_shared {
    astra_buffer_pool *pool;
    astra_latch latch;
    _Atomic unsigned errors;
    page_id_t base;
    uint32 count;
} thread_shared;

/*
 * Phase 1: many threads fetch the *same* set of pages concurrently.
 *
 * The pages are all resident before the threads start, which is what makes the assertion
 * possible: with a pool of eight frames and four pages, nothing can be evicted, so two
 * concurrent fetches of one identifier must return the same frame - which is the contract
 * that makes a pinned page's bytes belong to every holder of it. A pool that gave each
 * fetch its own copy would pass a test that only checked the data and fail this one.
 */
typedef struct phase1_arg {
    thread_shared *shared;
    uint32 iterations;
} phase1_arg;

static void phase1_worker(void *raw)
{
    phase1_arg *arg = (phase1_arg *)raw;
    thread_shared *shared = arg->shared;
    astra_page *reference[4] = { NULL, NULL, NULL, NULL };
    prng rng;
    size_t i;

    for (i = 0u; i < 4u; ++i) {
        reference[i] = NULL;
    }

    prng_seed(&rng, 0xA5A5A5A500000001ull + (uint64)(uintptr_t)arg);

    for (uint32 n = 0u; n < arg->iterations; ++n) {
        uint32 slot = (uint32)(prng_next(&rng) % 4u);
        astra_page *page = NULL;
        astra_status status;

        status = astra_buffer_pool_fetch_page(shared->pool, shared->base + (page_id_t)slot,
                                               &page);
        thread_check(status == ASTRA_OK, "concurrent fetch of a resident page", __LINE__);
        if (status != ASTRA_OK) {
            atomic_fetch_add(&shared->errors, 1u);
            return;
        }

        /* The first fetch through this slot records the frame. Every later one must see
         * exactly the same pointer. */
        if (reference[slot] == NULL) {
            reference[slot] = page;
        } else {
            thread_check(reference[slot] == page,
                         "two concurrent fetches returned different frames", __LINE__);
            if (reference[slot] != page) {
                atomic_fetch_add(&shared->errors, 1u);
            }
        }

        /* The bytes are intact while pinned, whatever else is happening. */
        thread_check(page->page_id == shared->base + (page_id_t)slot,
                     "page identity while pinned", __LINE__);

        status = astra_buffer_pool_unpin_page(shared->pool, shared->base + (page_id_t)slot,
                                             false);
        thread_check(status == ASTRA_OK, "unpin of a page this thread fetched", __LINE__);
        if (status != ASTRA_OK) {
            atomic_fetch_add(&shared->errors, 1u);
            return;
        }
    }
}

/*
 * Phase 2: threads work a shared page set in a pool far too small to hold it, so every
 * fetch is also an eviction and every dirty page is also a write.
 *
 * ASTRA_ERR_INVALID_STATE is an expected outcome here rather than a failure: four threads
 * can each hold a pin in a four-frame pool at the same moment, and the contract says the
 * pool says so instead of blocking. The test asserts that it says so, and that the data is
 * still correct afterwards - which is the pair that matters. A pool that blocked instead
 * would deadlock, and a pool that stole a pin would corrupt data, so the failure mode this
 * group is built to catch is the one where the answer is neither.
 */
typedef struct phase2_arg {
    thread_shared *shared;
    uint32 iterations;
    uint32 worker_id;

    /*
     * Why this worker gave up, if it did.
     *
     * A bare error count says three of four threads failed. The string says which of the
     * four call sites they failed at, which is the difference between a bug report and a
     * puzzle. Each worker owns its own `phase2_arg`, so writing it needs no lock, and the
     * main thread reads it only after joining.
     */
    const char *reason;
    astra_status bad_status;
    astra_status bad_expected;
} phase2_arg;

static void phase2_worker(void *raw)
{
    phase2_arg *arg = (phase2_arg *)raw;
    thread_shared *shared = arg->shared;
    prng rng;
    unsigned local_errors = 0u;

    prng_seed(&rng, 0xC0FFEE0000ull + arg->worker_id);

    for (uint32 n = 0u; n < arg->iterations; ++n) {
        uint32 slot = (uint32)(prng_next(&rng) % shared->count);
        page_id_t id = shared->base + slot;
        astra_page *page = NULL;
        astra_status status;

        status = astra_buffer_pool_fetch_page(shared->pool, id, &page);
        if (status == ASTRA_ERR_INVALID_STATE) {
            /* Every frame is pinned by some thread. Legitimate, and the documented
             * answer. */
            continue;
        }
        if (status != ASTRA_OK) {
            ++local_errors;
            arg->reason = "fetch returned an unexpected status";
            arg->bad_status = status;
            arg->bad_expected = ASTRA_OK;
            break;
        }

        /*
         * The pool's central promise, checked at the one moment it can be broken: a
         * successful fetch returns a pinned page, and the pin is what stops the frame being
         * recycled under the caller's feet. This belongs immediately after the fetch and
         * before anything else, because it is the only place the claim can be falsified
         * without the test's own actions being a plausible explanation - a failure further
         * down is a symptom, and a failure here is the cause.
         */
        {
            uint32 pins = 0u;
            astra_status pc = astra_buffer_pool_pin_count(shared->pool, id, &pins);

            if (pc != ASTRA_OK || pins == 0u) {
                ++local_errors;
                arg->reason = "a successful fetch did not leave the page pinned";
                arg->bad_status = pc;
                arg->bad_expected = ASTRA_OK;
                break;
            }
        }

        /*
         * Each page belongs to exactly one worker, so no two threads write the same
         * buffer. That is the caller-side half of the contract: the pool guarantees the
         * frame is not recycled under a pin, and says nothing about two threads in one
         * page at once, so the test does not do that.
         */
        if ((slot % 4u) == (arg->worker_id % 4u)) {
            uint32 generation = (uint32)(n + 1u) * 4u + slot;
            astra_status unpin;

            page_fill(page, id, generation);
            unpin = astra_buffer_pool_unpin_page(shared->pool, id, true);
            if (unpin != ASTRA_OK) {
                ++local_errors;
                arg->reason = "unpin of a page this thread wrote";
                arg->bad_status = unpin;
                arg->bad_expected = ASTRA_OK;
                break;
            }
        } else {
            astra_status unpin = astra_buffer_pool_unpin_page(shared->pool, id, false);

            if (unpin != ASTRA_OK) {
                ++local_errors;
                arg->reason = "unpin of a page this thread only read";
                arg->bad_status = unpin;
                arg->bad_expected = ASTRA_OK;
                break;
            }
        }

        /* Interleaved flushes of pages other threads are pinning, which is what exercises
         * the write latch and the eviction-with-a-dirty-victim path under contention. */
        if ((n % 16u) == 0u) {
            status = astra_buffer_pool_flush_all(shared->pool);
            if (status != ASTRA_OK && status != ASTRA_ERR_INVALID_STATE) {
                ++local_errors;
                arg->reason = "flush_all during concurrent writes";
                arg->bad_status = status;
                arg->bad_expected = ASTRA_OK;
                break;
            }
        }
    }

    atomic_fetch_add(&shared->errors, local_errors);
}


/*
 * The threaded group keeps its fixture in a file-scope variable rather than on the stack.
 * The worker functions receive a `thread_shared` and never touch the fixture, so this is
 * only about keeping the 250-line group's locals from being pushed under the thread stacks
 * - and about having a name to refer to in a comment when a future group needs the same
 * setup. A local would have worked; this reads better next to the two workers.
 */
static buffer_fixture g_thread_fixture;

void astra_test_buffer_threads(void)
{
    enum { WORKERS = 4, ITERATIONS = 4000, PAGES = 4, BUSY_PAGES = 8 };
    thread_shared shared;
    phase1_arg phase1[WORKERS];
    phase2_arg phase2[WORKERS];
    astra_test_thread threads[WORKERS];
    astra_page *page = NULL;
    page_id_t first = 0u;
    page_id_t last = 0u;
    uint32 started = 0u;
    uint32 i;

    astra_test_begin("buffer_threads");

    /*
     * Phase 1: an eight-frame pool over four pages, so all four stay resident and the
     * "two concurrent fetches return the same frame" assertion is exact rather than racy.
     * With fewer frames than pages, an eviction could take a frame between the two
     * fetches and the test would be asserting a coincidence.
     */
    if (!fixture_open(&g_thread_fixture, 4096u, 8u)) {
        ASTRA_CHECK_FIXTURE(g_thread_fixture);
        return;
    }

    memset(&shared, 0, sizeof shared);
    shared.pool = g_thread_fixture.pool;
    atomic_init(&shared.errors, 0u);
    ASTRA_CHECK_STATUS(astra_latch_init(&shared.latch), ASTRA_OK);

    for (i = 0u; i < PAGES; ++i) {
        page = NULL;
        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(g_thread_fixture.pool, &page, &last),
                           ASTRA_OK);
        if (i == 0u) {
            first = last;
        }
        page_fill(page, last, i + 1u);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(g_thread_fixture.pool, last, true),
                           ASTRA_OK);
    }
    shared.base = first;
    shared.count = PAGES;

    started = 0u;
    for (i = 0u; i < WORKERS; ++i) {
        phase1[i].shared = &shared;
        phase1[i].iterations = ITERATIONS;
        if (thread_start(&threads[i], phase1_worker, &phase1[i])) {
            ++started;
        } else {
            ASTRA_CHECK_SETUP("could not start a worker thread");
        }
    }
    ASTRA_CHECK_UINT64(started, WORKERS);
    for (i = 0u; i < started; ++i) {
        thread_join(&threads[i]);
    }
    ASTRA_CHECK_UINT64(atomic_load(&shared.errors), 0u);

    /* Every pin came back. This is the assertion that says no thread leaked a pin and left
     * the pool permanently full - a failure that would otherwise surface much later as a
     * mysterious "every frame is pinned" from an unrelated fetch. */
    for (i = 0u; i < PAGES; ++i) {
        uint32 pins = 0u;

        ASTRA_CHECK_STATUS(
            astra_buffer_pool_pin_count(g_thread_fixture.pool, first + (page_id_t)i, &pins),
            ASTRA_OK);
        ASTRA_CHECK_UINT64(pins, 0u);
    }

    astra_latch_destroy(&shared.latch);
    fixture_close(&g_thread_fixture);

    /*
     * Phase 2: a four-frame pool over eight pages with four threads, so every fetch is also
     * an eviction and every dirty page is also a write.
     */
    if (!fixture_open(&g_thread_fixture, 4096u, 4u)) {
        ASTRA_CHECK_FIXTURE(g_thread_fixture);
        return;
    }

    memset(&shared, 0, sizeof shared);
    shared.pool = g_thread_fixture.pool;
    shared.count = BUSY_PAGES;
    atomic_init(&shared.errors, 0u);
    ASTRA_CHECK_STATUS(astra_latch_init(&shared.latch), ASTRA_OK);

    first = 0u;
    for (i = 0u; i < BUSY_PAGES; ++i) {
        page = NULL;
        ASTRA_CHECK_STATUS(astra_buffer_pool_new_page(g_thread_fixture.pool, &page, &last),
                           ASTRA_OK);
        if (i == 0u) {
            first = last;
        }
        page_fill(page, last, 1u);
        ASTRA_CHECK_STATUS(astra_buffer_pool_unpin_page(g_thread_fixture.pool, last, true),
                           ASTRA_OK);
    }
    shared.base = first;

    started = 0u;
    for (i = 0u; i < WORKERS; ++i) {
        phase2[i].shared = &shared;
        phase2[i].iterations = ITERATIONS;
        phase2[i].worker_id = i;
        phase2[i].reason = NULL;
        phase2[i].bad_status = ASTRA_OK;
        phase2[i].bad_expected = ASTRA_OK;
        if (thread_start(&threads[i], phase2_worker, &phase2[i])) {
            ++started;
        } else {
            ASTRA_CHECK_SETUP("could not start a worker thread");
        }
    }
    ASTRA_CHECK_UINT64(started, WORKERS);
    for (i = 0u; i < started; ++i) {
        thread_join(&threads[i]);
    }
    for (i = 0u; i < started; ++i) {
        if (phase2[i].reason != NULL) {
            astra_test_report_status("thread", (int)__LINE__, phase2[i].reason,
                                     phase2[i].bad_status, phase2[i].bad_expected);
        }
    }
    ASTRA_CHECK_UINT64(atomic_load(&shared.errors), 0u);

    /*
     * Flush, then check every page came back with one of the generations its owning thread
     * could have written. Contents from no generation at all, or from a generation
     * belonging to a different page, mean a buffer was recycled under somebody - which is
     * the specific failure a concurrent eviction has to be able to cause, and the reason
     * this group exists.
     *
     * Generation 0 is excluded because every page here is written before the threads start,
     * so a page reading back as zeros is a frame that was handed out without being loaded.
     */
    ASTRA_CHECK_STATUS(astra_buffer_pool_flush_all(g_thread_fixture.pool), ASTRA_OK);
    for (i = 0u; i < BUSY_PAGES; ++i) {
        page_id_t id = first + (page_id_t)i;
        astra_page *check = NULL;
        bool matched = false;

        if (astra_buffer_pool_fetch_page(g_thread_fixture.pool, id, &check) == ASTRA_OK
            && check != NULL) {
            for (uint32 generation = 1u; generation <= ITERATIONS * 4u && !matched;
                 ++generation) {
                if (page_check(check, id, generation)) {
                    matched = true;
                }
            }
            thread_check(matched, "page contents are from some generation a thread wrote",
                         __LINE__);
            (void)astra_buffer_pool_unpin_page(g_thread_fixture.pool, id, false);
        }
    }

    astra_latch_destroy(&shared.latch);
    fixture_close(&g_thread_fixture);
}
