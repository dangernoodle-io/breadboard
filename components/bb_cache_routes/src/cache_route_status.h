#pragma once
// cache_route_status — pure bb_err_t -> HTTP status mapper for the
// GET /api/cache handler (bb_cache_routes). No FreeRTOS/ESP-IDF/httpd types
// — host-testable in isolation, mirroring sse_pool_reclaim_decision.h.
//
// The ESP-IDF-only handler (platform/espidf/bb_cache_routes/bb_cache_routes.c)
// calls bb_cache_get_serialized() and delegates the resulting bb_err_t ->
// HTTP-status decision to cache_route_map_status() here, so Coveralls sees
// and the host test suite exercises every branch of that mapping even though
// the caller's own inputs (a live bb_cache registry) cannot be host-compiled.
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
//   BB_ERR_NO_SPACE       -> 500 (serialize/buffer failure)
//   any other             -> 500
// Any 500 status is logged loud on the caller side.
int cache_route_map_status(bb_err_t rc);

#ifdef __cplusplus
}
#endif
