#pragma once
// Portable typed lock-copy helper — POSIX pthread (host) and ESP-IDF POSIX layer.
#include <pthread.h>

// BB_LOCKED_COPY — acquire mtx_ptr, copy src to dst via typed struct assignment, release.
// Only for single-assignment critical sections; do not use when multiple operations
// must run atomically under the same lock.
#define BB_LOCKED_COPY(mtx_ptr, dst, src) do { \
    pthread_mutex_lock(mtx_ptr);                \
    (dst) = (src);                              \
    pthread_mutex_unlock(mtx_ptr);              \
} while (0)

// ---------------------------------------------------------------------------
// bb_lock — contention-instrumented opaque lock primitive
// ---------------------------------------------------------------------------
//
// A second, independent API on top of the same header: an opaque mutex
// handle (hides pthread_mutex_t on host, a FreeRTOS SemaphoreHandle_t on
// ESP-IDF — no platform type appears here) with optional acquisition/
// contention/wait/hold-time instrumentation.
//
// Two-level enable, both required for stats to actually accrue:
//   1. Compile gate BB_LOCK_STATS_ENABLE (bridged from CONFIG_BB_LOCK_STATS_ENABLE,
//      default n) — when off, bb_lock_lock/unlock compile to plain mutex
//      lock/unlock with zero instrumentation code, and bb_lock_get_stats
//      always returns a zero-filled struct.
//   2. Runtime flag bb_lock_stats_set_enabled()/bb_lock_stats_enabled() — when
//      the compile gate is on but the runtime flag is off, one atomic-bool
//      load per lock/unlock call is the only added cost; stats still read
//      back as zero.
//
// Contention accounting: contention_count is incremented only when lock()
// has to actually block (trylock-first: bb_lock_lock tries bb_lock_trylock
// first; a fast uncontended acquire is not contention). A failed
// bb_lock_trylock() call from the caller is NEVER contention — it is the
// caller's own explicit non-blocking probe.
#include <stdatomic.h>
#include <stddef.h>
#include <stdbool.h>
#include "bb_core.h"

// ---------------------------------------------------------------------------
// Kconfig bridge for BB_LOCK_STATS_ENABLE
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#  include "sdkconfig.h"
#  ifdef CONFIG_BB_LOCK_STATS_ENABLE
#    undef  BB_LOCK_STATS_ENABLE  /* suppress -Wmacro-redefined when build_flags and Kconfig both define this */
#    define BB_LOCK_STATS_ENABLE CONFIG_BB_LOCK_STATS_ENABLE
#  endif
#endif
#ifndef BB_LOCK_STATS_ENABLE
#define BB_LOCK_STATS_ENABLE 0
#endif

#define BB_LOCK_NAME_MAX     32
#define BB_LOCK_CATEGORY_MAX 24

// Bytes of in-place storage for the backend mutex object (pthread_mutex_t on
// host/ESP-IDF POSIX layer, or a FreeRTOS SemaphoreHandle_t). Sized generously
// above the largest observed pthread_mutex_t (64 bytes on macOS/BSD libc; ~40
// on glibc/musl); the backend .c files _Static_assert this at compile time.
#define BB_LOCK_IMPL_STORAGE_BYTES 64

#ifdef __cplusplus
extern "C" {
#endif

// bb_lock_init() config — config-struct registration convention (bb_cache_register
// precedent): extend by adding fields, never break the signature.
typedef struct {
    const char *name;     // human-readable identifier, copied into the handle (may be NULL)
    const char *category; // optional grouping label, copied into the handle (may be NULL)
} bb_lock_config_t;

// Opaque lock handle. Callers embed this by value (e.g. as a struct field or
// static) — bb_lock_init() populates it in place; no heap allocation.
typedef struct {
    union {
        max_align_t bb_lock_align; // forces alignment suitable for any backend mutex type
        unsigned char bb_lock_bytes[BB_LOCK_IMPL_STORAGE_BYTES];
    } bb_lock_impl;
    char name[BB_LOCK_NAME_MAX];
    char category[BB_LOCK_CATEGORY_MAX];
    _Atomic uint32_t bb_lock_acquisition_count;
    _Atomic uint32_t bb_lock_contention_count;
    _Atomic uint64_t bb_lock_wait_time_total_us;
    _Atomic uint64_t bb_lock_wait_time_max_us;
    _Atomic uint64_t bb_lock_hold_time_total_us;
    _Atomic uint64_t bb_lock_hold_time_max_us;
    _Atomic uint64_t bb_lock_held_since_us; // 0 = not currently held
    _Atomic bool bb_lock_initialized; // set true by bb_lock_init(); never true on a
                                       // zero-initialized handle that skipped init
    _Atomic bool bb_lock_destroyed;   // set true by the first successful bb_lock_destroy()
} bb_lock_t;

// Point-in-time snapshot returned by bb_lock_get_stats(). All-zero when
// BB_LOCK_STATS_ENABLE is 0 or the runtime flag is disabled.
typedef struct {
    uint32_t acquisition_count;
    uint32_t contention_count;
    uint64_t wait_time_total_us;
    uint64_t wait_time_max_us;
    uint64_t hold_time_total_us;
    uint64_t hold_time_max_us;
} bb_lock_stats_t;

// Initialize *out from *cfg. cfg->name/category are copied (truncated with
// explicit NUL-termination into BB_LOCK_NAME_MAX/BB_LOCK_CATEGORY_MAX) — the
// caller does not need to keep the strings alive past this call.
bb_err_t bb_lock_init(const bb_lock_config_t *cfg, bb_lock_t *out);

// Release backend resources. Safe to call on a zero-initialized handle that
// was never bb_lock_init()'d (no-op, returns BB_OK). Returns
// BB_ERR_INVALID_STATE — without touching the backend primitive — on a
// double-destroy (already destroyed) or a destroy attempted while the lock
// is currently held by another owner/waiter; re-invoking
// pthread_mutex_destroy()/vSemaphoreDelete() on an already-freed or held
// primitive is undefined behavior / corruption, so neither backend calls it
// in those cases.
//
// The held/double-destroy state check above is BEST-EFFORT, not atomic: a
// thread can acquire the lock in the window between this check and the
// backend destroy call, racing the destroy. The caller MUST guarantee no
// concurrent acquire is possible during destroy (e.g. by fully quiescing
// all other owners/waiters first) — bb_lock does not, and cannot, make
// check-then-destroy atomic across the two backend primitives it wraps.
bb_err_t bb_lock_destroy(bb_lock_t *lock);

// Blocking acquire. When stats are enabled, tries a non-blocking acquire
// first; if that fails, the wait is timed and contention_count/wait_time_*
// are updated once the blocking acquire succeeds.
bb_err_t bb_lock_lock(bb_lock_t *lock);

// Non-blocking acquire. Returns BB_OK on success, BB_ERR_TIMEOUT if already
// held by another owner. Never counts as contention (see header note above).
bb_err_t bb_lock_trylock(bb_lock_t *lock);

// Release a lock held by the caller.
bb_err_t bb_lock_unlock(bb_lock_t *lock);

// Copy a point-in-time snapshot of lock's stats into *out.
void bb_lock_get_stats(const bb_lock_t *lock, bb_lock_stats_t *out);

// Reset all counters on lock to zero (test isolation / periodic rollup).
void bb_lock_reset_stats(bb_lock_t *lock);

// Runtime enable/disable for stats collection (only takes effect when the
// BB_LOCK_STATS_ENABLE compile gate is on).
void bb_lock_stats_set_enabled(bool enabled);
bool bb_lock_stats_enabled(void);

#ifdef __cplusplus
}
#endif
