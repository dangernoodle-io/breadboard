// bb_lock — ESP-IDF backend: FreeRTOS mutex semaphore, contention-instrumented.
//
// Uses xSemaphoreCreateMutex() (NOT a binary semaphore) so the owning task
// gets priority inheritance — a binary semaphore has no owner concept and
// cannot boost a low-priority holder blocking a high-priority waiter.
//
// Stats bookkeeping is shared with the host backend via bb_lock_stats.c/.h
// (see that file); this file owns only the runtime enable/disable flag and
// the mutex API.

#include "bb_lock.h"
#include "bb_lock_stats.h"
#include "bb_clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdatomic.h>

_Static_assert(sizeof(SemaphoreHandle_t) <= BB_LOCK_IMPL_STORAGE_BYTES,
               "SemaphoreHandle_t exceeds bb_lock_t backend storage");

static inline SemaphoreHandle_t bb_lock_impl_get(bb_lock_t *lock)
{
    SemaphoreHandle_t h;
    memcpy(&h, lock->bb_lock_impl.bb_lock_bytes, sizeof(h));
    return h;
}

static inline void bb_lock_impl_set(bb_lock_t *lock, SemaphoreHandle_t h)
{
    memcpy(lock->bb_lock_impl.bb_lock_bytes, &h, sizeof(h));
}

#if BB_LOCK_STATS_ENABLE
static _Atomic bool s_stats_runtime_enabled = true;
#endif // BB_LOCK_STATS_ENABLE

bb_err_t bb_lock_init(const bb_lock_config_t *cfg, bb_lock_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    SemaphoreHandle_t h = xSemaphoreCreateMutex();
    if (!h) {
        return BB_ERR_NO_MEM;
    }
    bb_lock_impl_set(out, h);
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
        // Double-destroy: never re-invoke vSemaphoreDelete on an
        // already-freed primitive.
        return BB_ERR_INVALID_STATE;
    }
    SemaphoreHandle_t h = bb_lock_impl_get(lock);
    if (h && xSemaphoreGetMutexHolder(h) != NULL) {
        // Currently held by another owner/waiter — refuse to destroy under
        // a live holder.
        return BB_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&lock->bb_lock_destroyed, true, memory_order_release);
    if (h) {
        vSemaphoreDelete(h);
        bb_lock_impl_set(lock, NULL);
    }
    return BB_OK;
}

bb_err_t bb_lock_lock(bb_lock_t *lock)
{
    if (!lock) {
        return BB_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t h = bb_lock_impl_get(lock);
#if BB_LOCK_STATS_ENABLE
    if (atomic_load_explicit(&s_stats_runtime_enabled, memory_order_relaxed)) {
        if (xSemaphoreTake(h, 0) == pdTRUE) {
            bb_lock_stats_record_acquired(lock, 0);
            return BB_OK;
        }
        uint64_t wait_start = bb_clock_now_us();
        xSemaphoreTake(h, portMAX_DELAY);
        uint64_t wait_us = bb_clock_now_us() - wait_start;
        atomic_fetch_add_explicit(&lock->bb_lock_contention_count, 1u, memory_order_relaxed);
        bb_lock_stats_record_acquired(lock, wait_us);
        return BB_OK;
    }
#endif
    xSemaphoreTake(h, portMAX_DELAY);
    return BB_OK;
}

bb_err_t bb_lock_trylock(bb_lock_t *lock)
{
    if (!lock) {
        return BB_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t h = bb_lock_impl_get(lock);
    if (xSemaphoreTake(h, 0) != pdTRUE) {
        // Contended trylock is NOT contention — that only counts on a lock()
        // call that had to block.
        return BB_ERR_TIMEOUT;
    }
#if BB_LOCK_STATS_ENABLE
    if (atomic_load_explicit(&s_stats_runtime_enabled, memory_order_relaxed)) {
        bb_lock_stats_record_acquired(lock, 0);
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
    // Unconditionally attempt the release-side bookkeeping — see the
    // matching comment in platform/host/bb_core/bb_lock.c for the stale
    // held_since_us-across-a-runtime-toggle rationale.
    bb_lock_stats_record_released(lock);
#endif
    SemaphoreHandle_t h = bb_lock_impl_get(lock);
    return (xSemaphoreGive(h) == pdTRUE) ? BB_OK : BB_ERR_INVALID_STATE;
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
