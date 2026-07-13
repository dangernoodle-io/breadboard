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
//
// A LIVE (bb_lock_init()'d) bb_lock_t MUST NOT be copied by value —
// e.g. `bb_lock_t b = a;` after `bb_lock_init(&a, ...)`. Both handles would
// then share one underlying backend mutex object identity while looking
// like two independent locks — undefined behavior on lock/unlock/destroy.
// Only a zero-initialized (never bb_lock_init()'d) handle is safe to copy.
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

// ---------------------------------------------------------------------------
// bb_lock_cond — condition variable composed with bb_lock_t
// ---------------------------------------------------------------------------
//
// Standard mutex+condvar pairing. bb_lock_cond_wait() ATOMICALLY releases the
// caller-supplied bb_lock_t and blocks the calling thread until signaled,
// broadcast, or timeout_ms elapses, then RE-ACQUIRES the same lock before
// returning — the caller must hold `lock` when calling wait()/signal()/
// broadcast() (standard condvar contract) and continues to hold it (or, on
// wait(), holds it again) on every return path.
//
// bb_lock_cond_signal() wakes at most one blocked waiter (unspecified which);
// bb_lock_cond_broadcast() wakes ALL current waiters. Broadcast is not
// optional decoration here — any consumer with more than one concurrent
// waiter on the same condition (e.g. a multi-peeker mailbox) MUST use it:
// signal() alone leaves every waiter but one blocked until its own timeout.
//
// Spurious wakeups are POSSIBLE (both POSIX and this primitive's own
// implementations permit them) — callers MUST loop on their own predicate
// after wait() returns BB_OK, never assume a single wait() call means the
// condition actually holds.
//
// HAZARD — timeout erosion: timeout_ms is a RELATIVE duration, re-armed in
// FULL on every call. The naive predicate loop this file's own spurious-
// wakeup note above implies —
//     while (!pred) { rc = bb_lock_cond_wait(cond, lock, timeout_ms); }
// — silently turns a bounded wait UNBOUNDED: every spurious wakeup restarts
// the full timeout_ms window from zero, so a caller can block far longer
// than the timeout it asked for (in the worst case, forever, under a
// sufficiently adversarial spurious-wakeup/signal cadence). The correct
// idiom computes an absolute deadline ONCE, before the loop, and passes the
// REMAINING time on every iteration:
//     uint64_t deadline_us = bb_clock_now_us() + (uint64_t)timeout_ms * 1000;
//     bb_err_t rc = BB_OK;
//     while (!pred) {
//         uint64_t now_us = bb_clock_now_us();
//         if (now_us >= deadline_us) { rc = BB_ERR_TIMEOUT; break; }
//         uint32_t remaining_ms = (uint32_t)((deadline_us - now_us) / 1000);
//         rc = bb_lock_cond_wait(cond, lock, remaining_ms);
//         if (rc == BB_ERR_TIMEOUT) { break; }
//     }
// (bb_clock_now_us() is the project's canonical monotonic clock helper, see
// bb_clock.h — never hand-roll esp_timer_get_time()/1000 here.) Any consumer
// of this primitive (e.g. B1-821's bb_bqueue) MUST use the
// absolute-deadline/remaining-time idiom above, never the naive re-loop.
//
// timeout_ms is in MILLISECONDS, never raw platform ticks.
// BB_LOCK_COND_WAIT_FOREVER blocks with no timeout. bb_lock_cond_wait()
// returns BB_ERR_TIMEOUT (with the lock re-acquired) if timeout_ms elapses
// with no signal/broadcast.
//
// Storage: caller-owned, no heap, in-place BB_LOCK_COND_IMPL_STORAGE_BYTES
// backend storage — same convention as bb_lock_t above.
//
// Backend (B1-822):
//   host    — POSIX pthread_cond_t paired with the SAME pthread_mutex_t
//             bb_lock_t already wraps on host (see bb_lock_impl.h). Timeouts
//             use CLOCK_MONOTONIC (via pthread_condattr_setclock()) on
//             Linux/glibc/musl; macOS has no pthread_condattr_setclock(), so
//             there this backend uses the Darwin-only
//             pthread_cond_timedwait_relative_np() extension (a RELATIVE
//             timeout, immune to wall-clock steps by construction) instead
//             of computing an absolute deadline. See bb_lock_cond.c.
//   ESP-IDF — a FreeRTOS-native waiter-list implementation (NOT ESP-IDF's own
//             pthread_cond_t): bb_lock_t's ESP-IDF backend deliberately wraps
//             a raw FreeRTOS mutex semaphore rather than a pthread_mutex_t
//             (see bb_lock.c's priority-inheritance rationale), so
//             pthread_cond_wait()/pthread_cond_timedwait() — which require a
//             pthread_mutex_t* — cannot pair with it. This backend instead
//             composes with bb_lock_t through its PUBLIC bb_lock_lock()/
//             bb_lock_unlock() API, using the same waiter-list ALGORITHM
//             ESP-IDF's own components/pthread/pthread_cond_var.c uses (a
//             list of per-waiter counting semaphores; broadcast walks the
//             whole list) — a proven design, not a novel one. Each waiter's
//             semaphore is a stack-resident StaticSemaphore_t: zero heap in
//             the wait() hot path; only the one-time per-condvar guard mutex
//             (bb_lock_cond_init) is heap-allocated, matching bb_lock's own
//             xSemaphoreCreateMutex() convention. See bb_lock_cond.c.
//
// Out of scope (B1-524, deliberately not built here): stats/instrumentation
// on this primitive, semaphore consolidation, any broader mutex/sem/condvar
// unification. Note this is ESP-IDF-only in practice: the host backend
// composes with bb_lock_t via the raw pthread_mutex_t bb_lock_impl.h exposes
// (bypassing bb_lock_lock()/unlock()'s public API and its stats
// instrumentation entirely, not merely leaving it disabled), so
// lock-contention stats around a condvar wait's lock reacquire are only ever
// observable on the ESP-IDF backend, never on host.
//
// ESP-IDF timeout range: bb_lock_cond_wait()'s timeout_ms is converted to
// FreeRTOS ticks internally with saturating (never wrapping) 64-bit math —
// a large-but-finite timeout_ms clamps to (portMAX_DELAY - 1) ticks rather
// than silently becoming a much shorter wait or an unbounded one. See
// bb_lock_cond_wait()'s own doc comment below for the caller-visible
// contract.

// Bytes of in-place storage for the backend condvar object (pthread_cond_t
// on host — 48 bytes on glibc/musl/macOS — or a small FreeRTOS waiter-list
// header on ESP-IDF); sized generously above both. Backend .c files
// _Static_assert this at compile time.
#define BB_LOCK_COND_IMPL_STORAGE_BYTES 64

// timeout_ms sentinel for bb_lock_cond_wait(): block with no timeout.
#define BB_LOCK_COND_WAIT_FOREVER 0xFFFFFFFFu

// Opaque condvar handle. Callers embed this by value — bb_lock_cond_init()
// populates it in place; no heap allocation for the handle itself (see the
// per-backend note above for each backend's own internal allocation, if any).
//
// A LIVE (bb_lock_cond_init()'d) bb_lock_cond_t MUST NOT be copied by
// value — two handles would then share one underlying backend guard/
// pthread_cond_t identity while looking like two independent condvars —
// undefined behavior on wait()/signal()/broadcast()/destroy(). Only a
// zero-initialized (never bb_lock_cond_init()'d) handle is safe to copy.
// Mirrors the same constraint on bb_lock_t above.
typedef struct {
    union {
        max_align_t bb_lock_cond_align;
        unsigned char bb_lock_cond_bytes[BB_LOCK_COND_IMPL_STORAGE_BYTES];
    } bb_lock_cond_impl;
    _Atomic bool bb_lock_cond_initialized; // set true by bb_lock_cond_init()
    _Atomic bool bb_lock_cond_destroyed;   // set true by the first successful bb_lock_cond_destroy()
} bb_lock_cond_t;

// Initialize *out. No heap allocation for the handle itself.
bb_err_t bb_lock_cond_init(bb_lock_cond_t *out);

// Release backend resources. Safe to call on a zero-initialized handle that
// was never bb_lock_cond_init()'d (no-op, returns BB_OK). Returns
// BB_ERR_INVALID_STATE on a double-destroy without touching the backend
// primitive again. Per POSIX condvar contract, it is undefined behavior to
// destroy a condvar with threads currently blocked in wait() on it — the
// caller MUST guarantee no waiter is blocked before calling this (bb_lock_cond
// does not, and cannot, check that cheaply/portably across backends).
//
// This also forbids a CONCURRENT in-flight signal()/broadcast() call, even
// when zero waiters are blocked: both touch the backend's internal guard
// primitive (e.g. the ESP-IDF backend's guard semaphore), and destroying
// that primitive out from under a signaller mid-call (e.g.
// vSemaphoreDelete() racing another task blocked taking the same semaphore)
// is undefined behavior in FreeRTOS, not merely a POSIX condvar-blocked-
// waiter concern. The caller MUST fully quiesce all signal()/broadcast()
// callers, not just waiters, before calling this.
bb_err_t bb_lock_cond_destroy(bb_lock_cond_t *cond);

// bb_lock_cond_wait()/signal()/broadcast() MUST NOT be called from ISR
// context on any backend — both use blocking primitives (xSemaphoreTake with
// a real timeout / xSemaphoreGive against a possibly-contended guard on
// ESP-IDF, pthread_cond_* on host), none of which are ISR-safe. Callers in
// interrupt handlers must defer to task context (e.g. via a queue or
// deferred-work mechanism) before touching a bb_lock_cond_t.

// Atomically release `lock` and block until signaled/broadcast or timeout_ms
// elapses, then re-acquire `lock` before returning. Caller MUST hold `lock`
// on entry. Returns BB_OK if woken (spurious or real — the caller MUST
// re-check its own predicate), BB_ERR_TIMEOUT if timeout_ms elapsed first.
// `lock` is re-acquired on every return path, including BB_ERR_TIMEOUT.
//
// BB_ERR_TIMEOUT is a RELIABLE "not signalled" observation, not merely a
// race-prone one: every backend guarantees this waiter was never popped by
// any signal()/broadcast() call before reporting BB_ERR_TIMEOUT, i.e. no
// signal was silently consumed and then misreported to this waiter as a
// timeout. A caller may therefore safely treat BB_ERR_TIMEOUT as "the
// condition was not signalled during this wait" and loop/retry (subject to
// the timeout-erosion hazard documented above), without needing any
// additional out-of-band confirmation that a wakeup was not lost.
bb_err_t bb_lock_cond_wait(bb_lock_cond_t *cond, bb_lock_t *lock, uint32_t timeout_ms);

// Wake at most one blocked waiter (unspecified which, if more than one).
bb_err_t bb_lock_cond_signal(bb_lock_cond_t *cond);

// Wake every currently blocked waiter. MANDATORY for any consumer with more
// than one concurrent waiter — see the header-level note above.
bb_err_t bb_lock_cond_broadcast(bb_lock_cond_t *cond);

#ifdef __cplusplus
}
#endif
