#pragma once

#include "bb_timer.h"
#include <stdbool.h>

/**
 * Host-only test hooks for deferred (MODE A) and worker (MODE B) timers.
 * Only available when BB_TIMER_TESTING is defined.
 */

/* MODE A deferred periodic: simulate timer tick. Coalesces: if already
 * pending, returns without calling work_fn. In sync mode (BB_TIMER_HOST_SYNC),
 * drains immediately. */
void bb_timer_deferred_periodic_fire_for_test(bb_periodic_timer_t t);

/* Manually set or clear the pending flag for coalesce tests. */
void bb_timer_deferred_set_pending_for_test(bb_periodic_timer_t t, bool v);

/* Drain a pending deferred periodic invocation synchronously. */
void bb_timer_deferred_drain_for_test(bb_periodic_timer_t t);

/* MODE A deferred one-shot: simulate timer expiry. Fires only if armed;
 * disarms and calls work_fn. */
void bb_timer_deferred_oneshot_fire_for_test(bb_oneshot_timer_t t);

/* MODE B worker: call work_fn synchronously on the calling thread. */
void bb_timer_worker_periodic_fire_for_test(bb_periodic_timer_t t);

/* Injectable malloc for bb_timer_deferred_periodic_create and
 * bb_timer_worker_periodic_create paths. NULL reverts to libc malloc. */
void bb_timer_set_malloc_for_test(void *(*fn)(size_t));

/* Toggle sync mode (fires work_fn inline in fire_for_test). */
void bb_timer_host_set_sync_mode(bool v);
