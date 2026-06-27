#pragma once
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *bb_timer_handle_t;
typedef void (*bb_timer_cb_t)(void *arg);

typedef enum {
    BB_TIMER_ONE_SHOT,
    BB_TIMER_PERIODIC,
} bb_timer_type_t;

bb_err_t bb_timer_create(const char *name, bb_timer_type_t type,
                         uint64_t period_us, bb_timer_cb_t cb, void *arg,
                         bb_timer_handle_t *out);
bb_err_t bb_timer_start(bb_timer_handle_t h);
bb_err_t bb_timer_stop(bb_timer_handle_t h);
bb_err_t bb_timer_delete(bb_timer_handle_t h);

// Return the current monotonic time in microseconds.
// Backed by esp_timer_get_time() on ESP-IDF and clock_gettime(CLOCK_MONOTONIC)
// on host/Arduino.
uint64_t bb_timer_now_us(void);

// ---------------------------------------------------------------------------
// Periodic timer API — portable wrapper around esp_timer periodic scheduling.
// ---------------------------------------------------------------------------

// Opaque periodic-timer handle.
typedef struct bb_periodic_timer *bb_periodic_timer_t;

// Create a periodic timer. `cb(arg)` fires every period once started. `name`
// is for diagnostics (may be NULL). On BB_OK, *out holds the handle.
bb_err_t bb_timer_periodic_create(void (*cb)(void *arg), void *arg,
                                  const char *name, bb_periodic_timer_t *out);

// Start (or restart) firing every period_us microseconds.
bb_err_t bb_timer_periodic_start(bb_periodic_timer_t t, uint64_t period_us);

// Stop firing (restartable).
bb_err_t bb_timer_periodic_stop(bb_periodic_timer_t t);

// Stop and free. Handle invalid after this returns.
bb_err_t bb_timer_periodic_delete(bb_periodic_timer_t t);

// ---------------------------------------------------------------------------
// One-shot timer API — portable wrapper around esp_timer one-shot scheduling.
// ---------------------------------------------------------------------------

// Opaque one-shot-timer handle.
typedef struct bb_oneshot_timer *bb_oneshot_timer_t;

// Create a one-shot timer. `cb(arg)` fires once, `delay_us` after start.
// `name` is for diagnostics (may be NULL). On BB_OK, *out holds the handle.
bb_err_t bb_timer_oneshot_create(void (*cb)(void *arg), void *arg,
                                 const char *name, bb_oneshot_timer_t *out);

// Arm: fire once after delay_us. Re-arming a pending/fired timer restarts it.
bb_err_t bb_timer_oneshot_start(bb_oneshot_timer_t t, uint64_t delay_us);

// Cancel a pending fire (no-op if already fired or not armed).
bb_err_t bb_timer_oneshot_stop(bb_oneshot_timer_t t);

// Cancel and free. Handle invalid after this returns.
bb_err_t bb_timer_oneshot_delete(bb_oneshot_timer_t t);

// ---------------------------------------------------------------------------
// MODE A: Deferred timer API — work_fn runs on shared bb_timer_disp task.
// Signal-only esp_timer cb + coalescing queue. For light housekeeping work.
// ---------------------------------------------------------------------------

// Create a deferred periodic timer. work_fn(arg) runs on the shared
// bb_timer_disp task (not the esp_timer service task). Coalesces: if a
// previous invocation is still pending, the second signal is dropped.
bb_err_t bb_timer_deferred_periodic_create(void (*work_fn)(void *arg), void *arg,
                                           const char *name, bb_periodic_timer_t *out);

// Create a deferred one-shot timer. work_fn(arg) runs on the shared
// bb_timer_disp task. Arms and disarms like bb_timer_oneshot_start/stop.
bb_err_t bb_timer_deferred_oneshot_create(void (*work_fn)(void *arg), void *arg,
                                          const char *name, bb_oneshot_timer_t *out);

// ---------------------------------------------------------------------------
// MODE B: Worker timer API — work_fn runs on a dedicated per-timer task.
// For heavy/hot/large-stack IO (TLS, mbedTLS handshake, etc.).
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t stack;    // 0 = default (4096)
    int      priority; // 0 = default (5)
    int      core;     // -1 = no affinity
} bb_timer_worker_cfg_t;

// Create a periodic timer with a dedicated worker task. work_fn(arg) runs on
// the dedicated task on each timer tick. cfg may be NULL for all defaults.
bb_err_t bb_timer_worker_periodic_create(void (*work_fn)(void *arg), void *arg,
                                         const char *name,
                                         const bb_timer_worker_cfg_t *cfg,
                                         bb_periodic_timer_t *out);

#ifdef __cplusplus
}
#endif
