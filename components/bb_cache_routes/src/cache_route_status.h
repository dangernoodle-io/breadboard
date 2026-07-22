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
//
// BB_ERR_UNSUPPORTED is newly REACHABLE here (B1-1053 PR1's bb_cache
// relaxation): a key registered with cfg->serialize == NULL (rendered via
// bb_data instead) makes bb_cache_get_serialized() return
// BB_ERR_UNSUPPORTED, which this mapper's "any other -> 500" catch-all turns
// into a loud 500 -- a known wart, not fixed here on purpose: bb_cache_routes
// is being folded into /api/diag/cache under B1-1121, so this mapping is
// being deleted, not polished. If you hit a spurious 500 on GET /api/cache
// for a NULL-serialize key, this is why.
int cache_route_map_status(bb_err_t rc);

#ifdef __cplusplus
}
#endif
