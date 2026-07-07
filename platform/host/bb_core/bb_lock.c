// bb_lock — host backend: POSIX pthread_mutex_t, contention-instrumented.
//
// Stats bookkeeping (BB_LOCK_STATS_ENABLE, two-level gate — see bb_lock.h)
// lives entirely in this file: per-lock atomics already sit inside bb_lock_t,
// so the only file-scope state needed here is the runtime enable/disable flag.

#include "bb_lock.h"
#include "bb_clock.h"
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>

_Static_assert(sizeof(pthread_mutex_t) <= BB_LOCK_IMPL_STORAGE_BYTES,
               "pthread_mutex_t exceeds bb_lock_t backend storage");

static inline pthread_mutex_t *bb_lock_impl(bb_lock_t *lock)
{
    return (pthread_mutex_t *)(void *)lock->bb_lock_impl.bb_lock_bytes;
}

#if BB_LOCK_STATS_ENABLE
static _Atomic bool s_stats_runtime_enabled = true;

static void bb_lock_record_acquired(bb_lock_t *lock, uint64_t wait_us)
{
    atomic_fetch_add_explicit(&lock->bb_lock_acquisition_count, 1u, memory_order_relaxed);
    if (wait_us > 0) {
        atomic_fetch_add_explicit(&lock->bb_lock_wait_time_total_us, wait_us, memory_order_relaxed);
        uint64_t cur_max = atomic_load_explicit(&lock->bb_lock_wait_time_max_us, memory_order_relaxed);
        // Retry loop: only re-enters when a concurrent updater's CAS races
        // this one between the load above and the CAS below. Host tests
        // exercise a single contended waiter per lock, so the retry path
        // (loop re-check + CAS-fail branch) is defensive-but-unreachable
        // without a second concurrent writer racing the exact same window;
        // not chased here (HIL-only equivalent tracked as B1-692).
        while (wait_us > cur_max) {  // LCOV_EXCL_BR_LINE — retry branch, see comment above
            if (atomic_compare_exchange_weak_explicit(&lock->bb_lock_wait_time_max_us, &cur_max, wait_us,  // LCOV_EXCL_BR_LINE — CAS-fail retry branch, see comment above
                                                       memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        }
    }
    atomic_store_explicit(&lock->bb_lock_held_since_us, bb_clock_now_us(), memory_order_relaxed);
}

static void bb_lock_record_released(bb_lock_t *lock)
{
    uint64_t since = atomic_exchange_explicit(&lock->bb_lock_held_since_us, 0, memory_order_relaxed);
    if (since == 0) {
        return;
    }
    uint64_t hold_us = bb_clock_now_us() - since;
    atomic_fetch_add_explicit(&lock->bb_lock_hold_time_total_us, hold_us, memory_order_relaxed);
    uint64_t cur_max = atomic_load_explicit(&lock->bb_lock_hold_time_max_us, memory_order_relaxed);
    while (hold_us > cur_max) {
        // CAS-fail retry branch: only taken when a concurrent updater races
        // this CAS between the load above and here — not host-reproducible
        // with a single-writer test lock; see the wait_time_max_us retry
        // loop comment above (HIL-only equivalent tracked as B1-692).
        if (atomic_compare_exchange_weak_explicit(&lock->bb_lock_hold_time_max_us, &cur_max, hold_us,  // LCOV_EXCL_BR_LINE — CAS-fail retry branch, see comment above
                                                   memory_order_relaxed, memory_order_relaxed)) {
            break;
        }
    }
}
#endif // BB_LOCK_STATS_ENABLE

bb_err_t bb_lock_init(const bb_lock_config_t *cfg, bb_lock_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    // LCOV_EXCL_START — pthread_mutex_init() failure (ENOMEM/EAGAIN/EPERM per
    // POSIX) is not host-reproducible without a libc fault-injection hook;
    // this is a defensive path, not a real branch the test suite can drive.
    // HIL-only equivalent tracked as B1-692.
    if (pthread_mutex_init(bb_lock_impl(out), NULL) != 0) {
        return BB_ERR_INVALID_STATE;
    }
    // LCOV_EXCL_STOP
    atomic_store_explicit(&out->bb_lock_initialized, true, memory_order_release);
    if (cfg) {
        if (cfg->name) {
            strncpy(out->name, cfg->name, BB_LOCK_NAME_MAX - 1);
            out->name[BB_LOCK_NAME_MAX - 1] = '\0';
        }
        if (cfg->category) {
            strncpy(out->category, cfg->category, BB_LOCK_CATEGORY_MAX - 1);
            out->category[BB_LOCK_CATEGORY_MAX - 1] = '\0';
        }
    }
    return BB_OK;
}

bb_err_t bb_lock_destroy(bb_lock_t *lock)
{
    if (!lock) {
        return BB_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&lock->bb_lock_initialized, memory_order_acquire)) {
        // Never bb_lock_init()'d (e.g. a zero-initialized handle) — safe no-op.
        return BB_OK;
    }
    if (atomic_load_explicit(&lock->bb_lock_destroyed, memory_order_acquire)) {
        // Double-destroy: never re-invoke pthread_mutex_destroy on an
        // already-freed primitive.
        return BB_ERR_INVALID_STATE;
    }
    pthread_mutex_t *m = bb_lock_impl(lock);
    if (pthread_mutex_trylock(m) != 0) {
        // Currently held by another owner/waiter — refuse to destroy under
        // a live holder.
        return BB_ERR_INVALID_STATE;
    }
    pthread_mutex_unlock(m);
    atomic_store_explicit(&lock->bb_lock_destroyed, true, memory_order_release);
    pthread_mutex_destroy(m);
    return BB_OK;
}

bb_err_t bb_lock_lock(bb_lock_t *lock)
{
    if (!lock) {
        return BB_ERR_INVALID_ARG;
    }
    pthread_mutex_t *m = bb_lock_impl(lock);
#if BB_LOCK_STATS_ENABLE
    if (atomic_load_explicit(&s_stats_runtime_enabled, memory_order_relaxed)) {
        if (pthread_mutex_trylock(m) == 0) {
            bb_lock_record_acquired(lock, 0);
            return BB_OK;
        }
        uint64_t wait_start = bb_clock_now_us();
        pthread_mutex_lock(m);
        uint64_t wait_us = bb_clock_now_us() - wait_start;
        atomic_fetch_add_explicit(&lock->bb_lock_contention_count, 1u, memory_order_relaxed);
        bb_lock_record_acquired(lock, wait_us);
        return BB_OK;
    }
#endif
    pthread_mutex_lock(m);
    return BB_OK;
}

bb_err_t bb_lock_trylock(bb_lock_t *lock)
{
    if (!lock) {
        return BB_ERR_INVALID_ARG;
    }
    pthread_mutex_t *m = bb_lock_impl(lock);
    if (pthread_mutex_trylock(m) != 0) {
        // Contended trylock is NOT contention — that only counts on a lock()
        // call that had to block.
        return BB_ERR_TIMEOUT;
    }
#if BB_LOCK_STATS_ENABLE
    if (atomic_load_explicit(&s_stats_runtime_enabled, memory_order_relaxed)) {
        bb_lock_record_acquired(lock, 0);
    }
#endif
    return BB_OK;
}

bb_err_t bb_lock_unlock(bb_lock_t *lock)
{
    if (!lock) {
        return BB_ERR_INVALID_ARG;
    }
#if BB_LOCK_STATS_ENABLE
    // Unconditionally attempt the release-side bookkeeping — do NOT gate
    // this on the current runtime flag. bb_lock_record_released() itself
    // no-ops when bb_lock_held_since_us is already 0 (the "was this acquire
    // instrumented" bit), so this is always safe; but skipping the call
    // when the runtime flag happens to be off at unlock time would leave a
    // stale nonzero held_since_us from an earlier tracked acquire in place.
    // If the flag is then flipped ON before the *next* acquire (which is
    // itself un-instrumented because the flag was off at lock() time), that
    // next unlock would read the ancient timestamp and compute a garbage
    // hold_us. Always clearing here closes that window.
    bb_lock_record_released(lock);
#endif
    // Failure branch (EPERM/EINVAL per POSIX — not the caller's owner, or a
    // corrupted primitive) is not host-reproducible on a correctly-used lock;
    // defensive path, HIL-only equivalent tracked as B1-692.
    return (pthread_mutex_unlock(bb_lock_impl(lock)) == 0) ? BB_OK : BB_ERR_INVALID_STATE;  // LCOV_EXCL_BR_LINE — see comment above
}

void bb_lock_get_stats(const bb_lock_t *lock, bb_lock_stats_t *out)
{
    if (!out) {
        return;
    }
#if BB_LOCK_STATS_ENABLE
    if (lock && atomic_load_explicit(&s_stats_runtime_enabled, memory_order_relaxed)) {
        out->acquisition_count  = atomic_load_explicit(&lock->bb_lock_acquisition_count,  memory_order_relaxed);
        out->contention_count   = atomic_load_explicit(&lock->bb_lock_contention_count,   memory_order_relaxed);
        out->wait_time_total_us = atomic_load_explicit(&lock->bb_lock_wait_time_total_us, memory_order_relaxed);
        out->wait_time_max_us   = atomic_load_explicit(&lock->bb_lock_wait_time_max_us,   memory_order_relaxed);
        out->hold_time_total_us = atomic_load_explicit(&lock->bb_lock_hold_time_total_us, memory_order_relaxed);
        out->hold_time_max_us   = atomic_load_explicit(&lock->bb_lock_hold_time_max_us,   memory_order_relaxed);
        return;
    }
#endif
    *out = (bb_lock_stats_t){0};
}

void bb_lock_reset_stats(bb_lock_t *lock)
{
    if (!lock) {
        return;
    }
#if BB_LOCK_STATS_ENABLE
    atomic_store_explicit(&lock->bb_lock_acquisition_count,  0u, memory_order_relaxed);
    atomic_store_explicit(&lock->bb_lock_contention_count,   0u, memory_order_relaxed);
    atomic_store_explicit(&lock->bb_lock_wait_time_total_us, 0,  memory_order_relaxed);
    atomic_store_explicit(&lock->bb_lock_wait_time_max_us,   0,  memory_order_relaxed);
    atomic_store_explicit(&lock->bb_lock_hold_time_total_us, 0,  memory_order_relaxed);
    atomic_store_explicit(&lock->bb_lock_hold_time_max_us,   0,  memory_order_relaxed);
#endif
}

void bb_lock_stats_set_enabled(bool enabled)
{
#if BB_LOCK_STATS_ENABLE
    atomic_store_explicit(&s_stats_runtime_enabled, enabled, memory_order_relaxed);
#else
    (void)enabled;
#endif
}

bool bb_lock_stats_enabled(void)
{
#if BB_LOCK_STATS_ENABLE
    return atomic_load_explicit(&s_stats_runtime_enabled, memory_order_relaxed);
#else
    return false;
#endif
}
