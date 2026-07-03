#pragma once
// sse_bundle_decision — pure decision logic for whether/how an SSE task
// bundle's static stack/TCB slot may be reissued to a new connection
// (B1-479/B1-484). No FreeRTOS/ESP-IDF types — host-testable in isolation.
//
// The ESP-IDF-only bb_pool SLOTS callbacks in bb_event_routes_espidf.c
// (sse_bundle_reusable/sse_bundle_reap) DO need real platform calls
// (eTaskGetState, vTaskDelete) that cannot run on host — but the actual
// *decision* they act on ("is this slot reusable, and does it need
// reaping first?") does not depend on FreeRTOS at all. Those callbacks
// translate the platform task's state into sse_bundle_task_state_t and
// delegate to sse_bundle_decide() here, so Coveralls can see and the host
// test suite can exercise every branch of that decision instead of it
// being invisible ESP-IDF-only logic.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSE_BUNDLE_TASK_NONE = 0,  // handle == NULL: bundle never assigned a task
    SSE_BUNDLE_TASK_RUNNING,   // task exists, has not yet reached eSuspended
    SSE_BUNDLE_TASK_SUSPENDED, // task exists and eTaskGetState == eSuspended
} sse_bundle_task_state_t;

typedef enum {
    SSE_BUNDLE_ACTION_ISSUE = 0,       // no prior task — hand the slot out as-is
    SSE_BUNDLE_ACTION_NOT_YET,         // still running — do not reissue yet
    SSE_BUNDLE_ACTION_REAP_THEN_ISSUE, // suspended corpse — reap, then reissue
} sse_bundle_action_t;

// Pure state -> action mapping. See the file header above for the mapping
// rationale; the caller's slot_reusable callback returns
// (action != SSE_BUNDLE_ACTION_NOT_YET), and slot_reap fires unconditionally
// on any true result (safe for SSE_BUNDLE_ACTION_ISSUE too, since the
// caller's reap implementation itself no-ops on a NULL task handle).
sse_bundle_action_t sse_bundle_decide(sse_bundle_task_state_t state);

#ifdef __cplusplus
}
#endif
