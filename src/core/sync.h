/*
 * AstraDB core: mutual exclusion and condition variables.
 *
 * Private. Not installed, not part of the public API. The public concurrency
 * contract of a subsystem is stated in that subsystem's own header; this header
 * only hides the fact that Windows and POSIX spell a mutex four different ways.
 *
 * Why it exists
 * -------------
 * The Buffer Pool is the first part of AstraDB that is accessed from more than one
 * thread, so it is the first part that needs a lock. Rather than write the
 * `#ifdef` pair in the middle of the page table, the two implementations live
 * here and every later subsystem - the WAL, the lock manager, a future page level
 * latch - uses the same three types.
 *
 * What it is and is not
 * ---------------------
 * It is a *non-recursive* mutual exclusion lock, a condition variable, and nothing
 * else. Deliberately absent, because each of them would be a design commitment
 * made before there is a user for it:
 *
 *   - No read/write lock. The Buffer Pool's latch guards a handful of words and is
 *     held for tens of nanoseconds; a reader/writer lock would add a second
 *     primitive to keep correct for no measurable gain.
 *   - No lock ordering or deadlock detection. A pool that holds two of these in a
 *     fixed order is the caller's responsibility to document, and the pool states
 *     its own order.
 *   - No timed waits, no try-lock, no shared/exclusive modes.
 *   - No lock-free anything. Correctness first: an uncontended `pthread_mutex_lock`
 *     is a compare-and-swap, and a broken lock-free algorithm is a data race that
 *     ThreadSanitizer may or may not happen to observe.
 *
 * Concurrency
 * -----------
 * An `astra_mutex` is not owned by any object in particular. It is initialised by
 * whoever creates the object that uses it and destroyed by whoever destroys that
 * object, in both cases exactly once, and it must not be copied: copying a held
 * lock copies a state that has no meaning, which is why the types are opaque
 * structs rather than open ones.
 *
 * An `astra_condvar` is always used with an `astra_mutex` that the caller holds.
 * The condition variable owns no state of its own beyond the platform's, so it is
 * always paired with the mutex it was created beside and is destroyed with it.
 *
 * Spurious wakeups are possible on both platforms, so every wait must be written
 * as
 *
 *     lock();
 *     while (!predicate) {
 *         astra_cond_wait(&cv, &mutex);
 *     }
 *     unlock();
 *
 * and never as an `if`. The Buffer Pool's waiters do exactly that, and the reason
 * is spelled out at each call site.
 *
 * Ownership
 * ---------
 * - `astra_mutex` and `astra_condvar` are caller owned values. They hold no heap
 *   memory, so they can live inside another object, but the object that embeds one
 *   is responsible for initialising and destroying it.
 * - Neither type is reference counted and neither is shared implicitly. Two objects
 *   that both embed the same `astra_mutex` are a bug in the caller, not a feature.
 * - `astra_mutex_destroy` and `astra_condvar_destroy` are required on POSIX and are
 *   no-ops on Windows, where the underlying primitives have no resources to
 *   release. They are required in both cases so that call sites read the same
 *   everywhere, and they tolerate a NULL argument for the same reason every other
 *   destructor in AstraDB does.
 */
#ifndef ASTRA_CORE_SYNC_H
#define ASTRA_CORE_SYNC_H

#include "astra/core/error.h"
#include "astra/core/types.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** A non-recursive mutual exclusion lock. Opaque; the layout is platform's. */
typedef struct astra_mutex {
#if defined(_WIN32)
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t handle;
#endif
} astra_mutex;

/** A condition variable. Always used with an `astra_mutex` held. */
typedef struct astra_condvar {
#if defined(_WIN32)
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t handle;
#endif
} astra_condvar;

/*
 * A mutex and the condition variable that belongs to it, as one value.
 *
 * They are always created and destroyed together and always used together - a
 * condition variable whose mutex is destroyed while a thread waits on it is
 * undefined behaviour on both platforms - so bundling them removes the way that
 * mistake gets made. The Buffer Pool embeds one of these.
 */
typedef struct astra_latch {
    /** Held for the whole of any critical section. */
    astra_mutex mutex;

    /** Signalled when `predicate` may have become true. */
    astra_condvar condvar;
} astra_latch;

/**
 * Initialises a mutex.
 *
 * Parameters:
 *   mutex - destination. Must not be NULL. Overwritten.
 *
 * Returns:
 *   ASTRA_OK on success.
 *   ASTRA_ERR_INVALID_ARGUMENT if `mutex` is NULL.
 *   ASTRA_ERR_INTERNAL if the platform refused to create the lock. There is no
 *           other outcome: an unusable lock is a broken process, not a condition a
 *           caller could recover from.
 *
 * Ownership: allocates nothing on the heap. `mutex` is owned by the caller and must
 * eventually be passed to astra_mutex_destroy exactly once.
 */
astra_status astra_mutex_init(astra_mutex *mutex);

/**
 * Releases a mutex's platform resources.
 *
 * Must not be called while the mutex is held or while another thread is waiting on
 * it. On Windows this releases nothing and always succeeds; on POSIX it destroys
 * the underlying `pthread_mutex_t`.
 *
 * Parameters:
 *   mutex - mutex to release. NULL is allowed and does nothing.
 *
 * Returns: ASTRA_OK on success. Never fails otherwise.
 *
 * Ownership: `mutex` must not be used afterwards.
 */
void astra_mutex_destroy(astra_mutex *mutex);

/** Acquires `mutex`, blocking until it is available. `mutex` must not be NULL. */
void astra_mutex_lock(astra_mutex *mutex);

/** Releases `mutex`, which the calling thread must currently hold. */
void astra_mutex_unlock(astra_mutex *mutex);

/**
 * Releases `mutex`, waits to be signalled on `condvar`, then re-acquires `mutex`.
 *
 * `mutex` must be held by the calling thread on entry and is held again on return.
 * The wait may return spuriously, so the caller must re-check its predicate in a
 * loop. Signalling `condvar` is always done while holding `mutex`.
 */
void astra_cond_wait(astra_condvar *condvar, astra_mutex *mutex);

/** Wakes every thread currently waiting on `condvar`. `mutex` need not be held. */
void astra_cond_broadcast(astra_condvar *condvar);

/**
 * Initialises a latch: its mutex and its condition variable together.
 *
 * Returns ASTRA_OK, or ASTRA_ERR_INVALID_ARGUMENT if `latch` is NULL.
 */
astra_status astra_latch_init(astra_latch *latch);

/**
 * Releases a latch's mutex and condition variable.
 *
 * Neither may be held or waited on. NULL is allowed and does nothing.
 */
void astra_latch_destroy(astra_latch *latch);

/** Locks `latch->mutex`. */
void astra_latch_lock(astra_latch *latch);

/** Unlocks `latch->mutex`. */
void astra_latch_unlock(astra_latch *latch);

/** Waits on `latch->condvar` with `latch->mutex` held, then re-acquires it. */
void astra_latch_wait(astra_latch *latch);

/** Broadcasts on `latch->condvar` to wake every waiter. */
void astra_latch_broadcast(astra_latch *latch);

#ifdef __cplusplus
}
#endif

#endif /* ASTRA_CORE_SYNC_H */
