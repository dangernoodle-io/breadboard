#pragma once
// bb_diag_cache_route_status — pure bb_err_t -> HTTP status mapper for the
// GET /api/diag/cache handler (bb_diag_routes.c). No FreeRTOS/ESP-IDF/httpd
// types -- host-testable in isolation, mirroring sse_pool_reclaim_decision.h.
//
// Folded in from the deleted bb_cache_routes component (B1-1121): this
// mapper used to back GET /api/cache; that route is now GET /api/diag/cache,
// same query-param shape, same {ts_ms,data} envelope body, handler moved
// into bb_diag_routes.c.
//
// The ESP-IDF-only handler (platform/espidf/bb_diag/bb_diag_routes.c) calls
// bb_cache_get_serialized() and delegates the resulting bb_err_t -> HTTP-
// status decision to bb_diag_cache_route_map_status() here, so Coveralls
// sees and the host test suite exercises every branch of that mapping even
// though the caller's own inputs (a live bb_cache registry) cannot be
// host-compiled.
//
// No "status + body" helper here (unlike a JSON-emitting component) --
// sse_pool_reclaim_decision.h (the mirrored precedent) only maps to an
// action/status value and leaves body construction to the caller; the
// handler builds its own error body via bb_http_resp_json_obj_* the same way
// events_handler() does for its error paths.

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pure bb_err_t -> HTTP status mapping:
//   BB_OK                -> 200 (snapshot found and copied)
//   BB_ERR_NOT_FOUND      -> 404 (key not registered)
//   BB_ERR_INVALID_STATE  -> 404 (key registered but no snapshot yet)
//   BB_ERR_UNSUPPORTED    -> 501 (key registered with cfg->serialize == NULL --
//                            it renders via bb_data, not this legacy
//                            serializer path; a config/wiring state, not a
//                            server fault -- distinct from both 404 cases
//                            above and from the "any other" 500 catch-all
//                            below, so a caller/operator can tell "this key
//                            will never work here" from "transient failure")
//   BB_ERR_NO_SPACE       -> 500 (serialize/buffer failure)
//   any other             -> 500
// Any 500 status is logged loud on the caller side.
int bb_diag_cache_route_map_status(bb_err_t rc);

#ifdef __cplusplus
}
#endif
