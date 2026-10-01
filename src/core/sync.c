#include "core/sync.h"

/*
 * Mutual exclusion and condition variables.
 *
 * There is nothing clever in this file and that is the point. The two platform
 * implementations are nine lines each, and keeping them here means the Buffer
 * Pool's page table can be written once, in terms of "latch", rather than once per
 * platform with the interesting part buried between the two.
 *
 * One choice worth naming: Windows gets CRITICAL_SECTION and CONDITION_VARIABLE
 * rather than SRWLOCK and the Win32 condition variable. SRWLOCK would have been
 * slimmer, and a slim-reader/writer lock is the wrong shape for a metadata latch
 * that is never held shared. More importantly SleepConditionVariableCS pairs with
 * CRITICAL_SECTION, so the Windows implementation is line for line the POSIX one
 * and there is no second protocol to reason about.
 *
 * Every lock in AstraDB has exactly one owner - the object that embeds it - and
 * that object initialises it in its constructor and destroys it in its destructor.
 * See the Ownership section of sync.h; the rule exists so that "who unlocks this"
 * has the same answer everywhere.
 */

/*
 * ---------------------------------------------------------------------------
 * Mutexes
 * ---------------------------------------------------------------------------
 */

astra_status astra_mutex_init(astra_mutex *mutex)
{
    if (mutex == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

#if defined(_WIN32)
    /*
     * InitializeCriticalSectionEx with a spin count of 0 makes the primitive a
     * plain critical section rather than one that spins before sleeping, which
     * matters because a short critical section is exactly what the Buffer Pool
     * takes, and spinning on a held lock in that case is pure waste.
     */
    if (!InitializeCriticalSectionEx(&mutex->cs, 0u, 0)) {
        return ASTRA_ERR_INTERNAL;
    }
#else
    /*
     * PTHREAD_MUTEX_DEFAULT is deliberately not PTHREAD_MUTEX_ERRORCHECK. Checking
     * would turn a recursive acquire, which is a deadlock, into a diagnosable
     * error at the cost of two extra branches on every acquisition in the hottest
     * path of the page table.
     */
    if (pthread_mutex_init(&mutex->handle, NULL) != 0) {
        return ASTRA_ERR_INTERNAL;
    }
#endif

    return ASTRA_OK;
}

void astra_mutex_destroy(astra_mutex *mutex)
{
    if (mutex == NULL) {
        return;
    }

#if !defined(_WIN32)
    (void)pthread_mutex_destroy(&mutex->handle);
#endif
}

void astra_mutex_lock(astra_mutex *mutex)
{
#if defined(_WIN32)
    EnterCriticalSection(&mutex->cs);
#else
    (void)pthread_mutex_lock(&mutex->handle);
#endif
}

void astra_mutex_unlock(astra_mutex *mutex)
{
#if defined(_WIN32)
    LeaveCriticalSection(&mutex->cs);
#else
    (void)pthread_mutex_unlock(&mutex->handle);
#endif
}

/*
 * ---------------------------------------------------------------------------
 * Condition variables
 * ---------------------------------------------------------------------------
 *
 * A wrapper would buy nothing here. There is no portable initialiser for a
 * pthread_cond_t, but the only thing to do is a parameterless init, so
 * astra_latch_init below performs it directly rather than exposing two more
 * functions whose bodies are a single call each.
 */

void astra_cond_wait(astra_condvar *condvar, astra_mutex *mutex)
{
#if defined(_WIN32)
    (void)SleepConditionVariableCS(&condvar->cv, &mutex->cs, INFINITE);
#else
    (void)pthread_cond_wait(&condvar->handle, &mutex->handle);
#endif
}

void astra_cond_broadcast(astra_condvar *condvar)
{
#if defined(_WIN32)
    WakeAllConditionVariable(&condvar->cv);
#else
    (void)pthread_cond_broadcast(&condvar->handle);
#endif
}

/*
 * ---------------------------------------------------------------------------
 * Latches
 * ---------------------------------------------------------------------------
 *
 * A latch is the unit that objects embed. The initialisers are the only place
 * where the two platform protocols differ beyond the primitive names, so they are
 * written out once here instead of at every construction site.
 */

astra_status astra_latch_init(astra_latch *latch)
{
    astra_status status;

    if (latch == NULL) {
        return ASTRA_ERR_INVALID_ARGUMENT;
    }

    status = astra_mutex_init(&latch->mutex);
    if (status != ASTRA_OK) {
        return status;
    }

#if defined(_WIN32)
    InitializeConditionVariable(&latch->condvar.cv);
#else
    if (pthread_cond_init(&latch->condvar.handle, NULL) != 0) {
        /*
         * The mutex is already live. Leaking it here would be the one place in
         * AstraDB where a partially constructed object escapes, so it is released
         * before returning and the caller is left with nothing to clean up.
         */
        astra_mutex_destroy(&latch->mutex);
        return ASTRA_ERR_INTERNAL;
    }
#endif

    return ASTRA_OK;
}

void astra_latch_destroy(astra_latch *latch)
{
    if (latch == NULL) {
        return;
    }

#if !defined(_WIN32)
    (void)pthread_cond_destroy(&latch->condvar.handle);
#endif
    astra_mutex_destroy(&latch->mutex);
}

void astra_latch_lock(astra_latch *latch)
{
    astra_mutex_lock(&latch->mutex);
}

void astra_latch_unlock(astra_latch *latch)
{
    astra_mutex_unlock(&latch->mutex);
}

void astra_latch_wait(astra_latch *latch)
{
    astra_cond_wait(&latch->condvar, &latch->mutex);
}

void astra_latch_broadcast(astra_latch *latch)
{
    astra_cond_broadcast(&latch->condvar);
}
