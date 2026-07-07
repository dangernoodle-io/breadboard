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

#include <sched.h>
#include <stdatomic.h>

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

#ifdef __cplusplus
}
#endif
