// bb_lock_cond — host backend: POSIX pthread_cond_t paired with bb_lock_t's
// underlying pthread_mutex_t (via bb_lock_impl.h — the same accessor bb_lock.c
// uses for the mutex API itself).
//
// CLOCK_MONOTONIC portability (B1-822): pthread_cond_timedwait() defaults to
// CLOCK_REALTIME, which a wall-clock step (NTP correction, manual date
// change, DST) silently corrupts mid-wait — this file explicitly requests
// CLOCK_MONOTONIC via pthread_condattr_setclock() wherever that call exists.
//
// macOS has no pthread_condattr_setclock() (Darwin's pthread_condattr_t has
// no clock-selection field) — there, pthread_cond_timedwait()'s absolute
// deadline argument is interpreted against the wall clock unconditionally,
// with the same step-corruption exposure. Darwin's portable workaround is
// pthread_cond_timedwait_relative_np(), a Darwin-only extension that takes a
// RELATIVE timespec (computed here from CLOCK_MONOTONIC, matching bb_clock's
// own host clock source) instead of an absolute deadline — immune to
// wall-clock steps because there is no absolute deadline to corrupt. This
// file therefore branches on __APPLE__: Linux/glibc/musl use the
// setclock+absolute-deadline path; Darwin uses the relative-timeout path.

#include "bb_lock.h"
#include "bb_lock_impl.h"
#include "bb_lock_cond_waiterlist.h"
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>

_Static_assert(sizeof(pthread_cond_t) <= BB_LOCK_COND_IMPL_STORAGE_BYTES,
               "pthread_cond_t exceeds bb_lock_cond_t backend storage");

static inline pthread_cond_t *bb_lock_cond_impl(bb_lock_cond_t *cond)
{
    return (pthread_cond_t *)(void *)cond->bb_lock_cond_impl.bb_lock_cond_bytes;
}

bb_err_t bb_lock_cond_init(bb_lock_cond_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#ifndef __APPLE__
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    // LCOV_EXCL_START — pthread_cond_init() failure (ENOMEM/EAGAIN per POSIX)
    // is not host-reproducible without a libc fault-injection hook; mirrors
    // bb_lock_init's identical defensive path. HIL-only equivalent: B1-692.
    if (pthread_cond_init(bb_lock_cond_impl(out), &attr) != 0) {
        pthread_condattr_destroy(&attr);
        return BB_ERR_INVALID_STATE;
    }
    // LCOV_EXCL_STOP
    pthread_condattr_destroy(&attr);
    atomic_store_explicit(&out->bb_lock_cond_initialized, true, memory_order_release);
    return BB_OK;
}

bb_err_t bb_lock_cond_destroy(bb_lock_cond_t *cond)
{
    if (!cond) {
        return BB_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&cond->bb_lock_cond_initialized, memory_order_acquire)) {
        // Never bb_lock_cond_init()'d — safe no-op.
        return BB_OK;
    }
    if (atomic_load_explicit(&cond->bb_lock_cond_destroyed, memory_order_acquire)) {
        // Double-destroy: never re-invoke pthread_cond_destroy on an
        // already-freed primitive.
        return BB_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&cond->bb_lock_cond_destroyed, true, memory_order_release);
    pthread_cond_destroy(bb_lock_cond_impl(cond));
    return BB_OK;
}

bb_err_t bb_lock_cond_wait(bb_lock_cond_t *cond, bb_lock_t *lock, uint32_t timeout_ms)
{
    if (!cond || !lock) {
        return BB_ERR_INVALID_ARG;
    }
    pthread_cond_t *c = bb_lock_cond_impl(cond);
    pthread_mutex_t *m = bb_lock_impl(lock);

    if (timeout_ms == BB_LOCK_COND_WAIT_FOREVER) {
        return (pthread_cond_wait(c, m) == 0) ? BB_OK : BB_ERR_INVALID_STATE;  // LCOV_EXCL_BR_LINE — pthread_cond_wait failure (EINVAL on misuse) not host-reproducible on a correctly paired lock/cond
    }

    int rc;
#ifdef __APPLE__
    struct timespec rel;
    rel.tv_sec = timeout_ms / 1000u;
    rel.tv_nsec = (long)(timeout_ms % 1000u) * 1000000L;
    rc = pthread_cond_timedwait_relative_np(c, m, &rel);
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec deadline = bb_lock_cond_deadline_from_now(now, timeout_ms);
    rc = pthread_cond_timedwait(c, m, &deadline);
#endif
    if (rc == ETIMEDOUT) {
        return BB_ERR_TIMEOUT;
    }
    return (rc == 0) ? BB_OK : BB_ERR_INVALID_STATE;  // LCOV_EXCL_BR_LINE — any other rc (EINVAL on misuse) not host-reproducible on a correctly paired lock/cond
}

bb_err_t bb_lock_cond_signal(bb_lock_cond_t *cond)
{
    if (!cond) {
        return BB_ERR_INVALID_ARG;
    }
    return (pthread_cond_signal(bb_lock_cond_impl(cond)) == 0) ? BB_OK : BB_ERR_INVALID_STATE;  // LCOV_EXCL_BR_LINE — pthread_cond_signal failure not host-reproducible
}

bb_err_t bb_lock_cond_broadcast(bb_lock_cond_t *cond)
{
    if (!cond) {
        return BB_ERR_INVALID_ARG;
    }
    return (pthread_cond_broadcast(bb_lock_cond_impl(cond)) == 0) ? BB_OK : BB_ERR_INVALID_STATE;  // LCOV_EXCL_BR_LINE — pthread_cond_broadcast failure not host-reproducible
}
