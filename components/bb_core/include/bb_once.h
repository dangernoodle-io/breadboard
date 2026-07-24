#pragma once

// bb_once — portable run-exactly-once primitive.
//
// A single bb_once_t guards a piece of work so that, across any number of
// concurrent callers, the supplied fn runs exactly one time; every other
// caller (whether racing the first call or arriving after it completed)
// blocks until that single run has finished, then returns immediately.
//
// Near-zero-dep leaf: C11 atomics everywhere; host/Arduino wait via
// sched_yield() (newlib/pthread), ESP-IDF waits via a real vTaskDelay(1)
// timed block (see rationale in bb_once_run below) — the only platform
// header pulled in is FreeRTOS, and only under #ifdef ESP_PLATFORM.
//
// NOTE for ESP-IDF consumers: sched_yield() (used on the host/Arduino path)
// is provided by ESP-IDF's pthread component — link it if you build this
// header outside the normal ESP-IDF component graph where it's already a
// transitive dependency.
//
// Usage:
//   static bb_once_t s_once = BB_ONCE_INIT;
//   bb_once_run(&s_once, my_init_fn, my_ctx);
//
// For a body that can FAIL (allocates/creates a platform resource), use the
// sibling bb_once_run_fallible() below instead — bb_once_run() latches DONE
// unconditionally, which permanently replays a transient failure; see
// bb_once_run_fallible()'s own doc comment for the full contract.

#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Internal states — not part of the public contract, do not compare directly.
#define BB_ONCE_STATE_IDLE    0
#define BB_ONCE_STATE_RUNNING 1
#define BB_ONCE_STATE_DONE    2

typedef struct {
    _Atomic int bb_once_state;
} bb_once_t;

// Static/compound-literal initializer for a bb_once_t.
#define BB_ONCE_INIT { BB_ONCE_STATE_IDLE }

// Run fn(ctx) exactly once across all callers sharing *once. The first caller
// to arrive runs fn synchronously on its own stack/context; every other
// caller (concurrent or later) blocks (yielding the CPU) until that run has
// completed, then returns without invoking fn. Safe to call repeatedly —
// every call after the first is a no-op.
static inline void bb_once_run(bb_once_t *once, void (*fn)(void *ctx), void *ctx)
{
    if (!once) {
        return;
    }

    int expected = BB_ONCE_STATE_IDLE;
    if (atomic_compare_exchange_strong(&once->bb_once_state, &expected,
                                        BB_ONCE_STATE_RUNNING)) {
        if (fn) {
            fn(ctx);
        }
        atomic_store(&once->bb_once_state, BB_ONCE_STATE_DONE);
        return;
    }

    while (atomic_load(&once->bb_once_state) != BB_ONCE_STATE_DONE) {
#ifdef ESP_PLATFORM
        // FreeRTOS vTaskDelay(0)/sched_yield() only yield to READY tasks of
        // the SAME priority — a higher-priority loser spinning here would
        // never let a lower-priority fn()-runner be scheduled at all
        // (classic priority-inversion livelock: the runner can't finish, so
        // the loser's wait condition never becomes true). vTaskDelay(1) is a
        // real timed block that hands the CPU to the scheduler regardless of
        // relative priority, so the runner (of any priority) gets to run.
        vTaskDelay(1);
#else
        // Host/Arduino: a preemptive OS scheduler (or Arduino's cooperative
        // loop) doesn't have FreeRTOS's strict same-priority-only yield
        // semantics, so sched_yield() is livelock-safe here.
        sched_yield();
#endif
    }
}

// bb_once_run_fallible — sibling of bb_once_run() for a FALLIBLE/allocating
// body (B1-524). bb_once_run() has no failure channel at all: it latches
// DONE unconditionally, which is the WRONG contract for a body that can
// transiently fail (e.g. xSemaphoreCreateMutex()/xQueueCreate() under heap
// pressure) — a transient failure would be latched as permanently "done",
// so every later caller replays that one failure forever instead of ever
// retrying. bb_once_run_fallible() fixes this: fn returns bool (true =
// succeeded, latch DONE; false = failed, reset back to IDLE) so the very
// next caller (whether a losing concurrent racer from this round or a
// wholly later call) genuinely re-attempts fn(), not a cached replay.
//
// Returns true if *once is (now, or already) DONE — i.e. some call's fn
// returned true; false if this call's own fn() attempt returned false (the
// caller should propagate its own error). A losing concurrent caller that
// observes another attempt fail and reset to IDLE does NOT itself retry —
// it returns false so its OWN caller can decide whether to retry (matching
// the natural "propagate this call's outcome" contract every other bb_
// error-returning API already uses); the next call from any thread will
// attempt fn() again from IDLE.
//
// Usage — the fallible/allocating creation runs INSIDE fn(), not in a
// separate bb_lock_t guarded by a plain bb_once_run(): guarding a fallible
// bb_lock_init() with bb_once_run() reproduces the exact same
// permanent-latch bug one level down (a failed bb_lock_init() leaves an
// uninitialized bb_lock_t that bb_once_run() nonetheless marks DONE forever
// -- see B1-524's bb_lifecycle_async.c precedent, which has this latent bug
// and is tracked separately). bb_once_run_fallible() needs no bb_lock_t at
// all: mutual exclusion between concurrent callers is the SAME
// compare-exchange + spin/yield-wait bb_once_run() already uses.
//
//   static bb_once_t s_once = BB_ONCE_INIT;
//   static bool my_fallible_init(void *ctx) {
//       s_handle = create_thing();
//       return s_handle != NULL;
//   }
//   if (!bb_once_run_fallible(&s_once, my_fallible_init, NULL) && !s_handle) {
//       return BB_ERR_NO_MEM; // this call's own attempt (or the racer whose
//                             // attempt it observed) failed; caller may retry
//   }
//
// CALLER CONTRACT — bounded wait, WDT-armed callers (B1-524 review): a
// loser's wait loop below is bounded ONLY under the single-active-caller-
// at-a-time composition-root contract this primitive's current consumers
// (bb_wifi_ping, bb_timer's disp_ensure_started, bb_mdns_init) all hold —
// no genuinely concurrent contention at the boot-time call sites this was
// built for. Under SUSTAINED failure with genuine concurrent contention,
// the guard can legitimately cycle RUNNING -> IDLE -> RUNNING indefinitely
// (each new attempt fails and resets), and a loser whose yield/vTaskDelay(1)
// wakeup always lands inside a RUNNING window could in principle spin
// without a hard cap. This is intentionally NOT capped here: a bounded
// retry-cap would force this function to sometimes return false for a
// winner attempt that was still RUNNING and about to succeed (the loser
// gave up first), which breaks the "false = this call's own attempt did
// not complete/observe a successful init" semantics documented above --
// the cap would have to lie about which outcome actually happened. Any
// caller invoked from a WDT-armed task that cannot rule out sustained
// concurrent failure MUST supply its OWN outer bound (e.g. a caller-side
// deadline before giving up and returning its own timeout error) rather
// than relying on this primitive to ever give up on its own.
static inline bool bb_once_run_fallible(bb_once_t *once, bool (*fn)(void *ctx), void *ctx)
{
    if (!once) {
        return false;
    }

    int expected = BB_ONCE_STATE_IDLE;
    if (atomic_compare_exchange_strong(&once->bb_once_state, &expected,
                                        BB_ONCE_STATE_RUNNING)) {
        bool ok = fn ? fn(ctx) : false;
        // Success latches DONE (never re-run again); failure resets to
        // IDLE so the next caller — not this one — gets to try again.
        atomic_store(&once->bb_once_state, ok ? BB_ONCE_STATE_DONE : BB_ONCE_STATE_IDLE);
        return ok;
    }

    for (;;) {
        int state = atomic_load(&once->bb_once_state);
        if (state == BB_ONCE_STATE_DONE) {
            return true;
        }
        if (state == BB_ONCE_STATE_IDLE) {
            // The attempt this call was waiting on failed and reset to
            // IDLE before this call observed RUNNING at all, or did so
            // just now — either way, no fn() ran on this call's behalf.
            // Report failure; a later call (this thread or another) will
            // pick up IDLE and attempt fn() again.
            return false;
        }
#ifdef ESP_PLATFORM
        vTaskDelay(1); // see bb_once_run()'s identical rationale above
#else
        sched_yield();
#endif
    }
}

#ifdef __cplusplus
}
#endif
