#pragma once
// sse_connect_error_decision — pure bb_err_t -> HTTP status mapping for the
// events_handler() acquire path in bb_event_routes_espidf.c (B1-561). No
// FreeRTOS/ESP-IDF types — host-testable in isolation, mirroring
// sse_bundle_decision.h / sse_pool_reclaim_decision.h.
//
// A transient BB_ERR_NO_SPACE (heap-pressure allocation failure, e.g. the
// lazy SSE task-stack pool's sse_task_bundles_ensure()) is retryable — the
// caller maps it to HTTP 503 so an EventSource client auto-retries the
// connect. Any other error (init failure, invalid state, etc.) is not
// transient and stays a hard HTTP 500.

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pure bb_err_t -> HTTP status mapping. Returns 503 for BB_ERR_NO_SPACE,
// 500 for anything else (including BB_OK, which callers should never pass
// here — a successful acquire has no error to map).
int sse_connect_error_status(bb_err_t err);

#ifdef __cplusplus
}
#endif
