#pragma once
// sse_pool_reclaim_decision — pure decision logic for whether the SSE
// task-bundle pool (s_sse_task_pool in bb_event_routes_espidf.c) may be torn
// down by the idle-reclaim tick (B1-492). No FreeRTOS/ESP-IDF/bb_pool types
// — host-testable in isolation, mirroring sse_bundle_decision.h.
//
// The ESP-IDF-only reclaim work_fn (bb_event_routes_espidf.c) first calls
// bb_pool_slots_reap_ready() to drain every corpse that has already reached
// eSuspended, then reads bb_event_routes_active_client_count(),
// bb_pool_slots_acquired_count(), and bb_pool_slots_pending_count(), and
// delegates the actual keep-vs-destroy call to sse_pool_reclaim_decide()
// here — so Coveralls sees and the host test suite exercises every branch of
// that decision even though the caller's own inputs (live task state, live
// client count) cannot be host-compiled.
//
// All three inputs are required, not just active_clients + pending_corpses:
// a bundle mid-teardown (its owning task has already decremented
// active_clients via bb_event_routes_client_release, but has not yet called
// bb_pool_release for its bundle) is neither an "active client" nor a
// "pending corpse" — it is still ACQUIRED (on loan). Destroying the pool
// while such a bundle is acquired would free a stack/TCB a FreeRTOS task is
// still actively executing on (use-after-free). acquired_slots closes this
// window.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSE_POOL_RECLAIM_KEEP = 0,  // clients, in-flight bundles, and/or not-yet-ready corpses remain
    SSE_POOL_RECLAIM_DESTROY,   // fully idle: 0 active clients, 0 acquired bundles, 0 pending corpses
} sse_pool_reclaim_action_t;

// Pure state -> action mapping. Destroy iff active_clients, acquired_slots,
// AND pending_corpses are all zero; keep the pool alive otherwise. Never
// gates SSE reuse itself (bb_pool_acquire's own reap-gate owns that) — this
// decision only governs the standing pool allocation.
sse_pool_reclaim_action_t sse_pool_reclaim_decide(size_t active_clients,
                                                  size_t acquired_slots,
                                                  size_t pending_corpses);

#ifdef __cplusplus
}
#endif
