#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "bb_core.h"
#include "bb_response.h"

// Compute the /api/health.ok gate: true when WiFi has IP AND OTA is validated.
// mDNS is intentionally excluded (locked decision B1-269).
bool bb_health_compute_ok(void);

// Register a named section for /api/health.
//
// name         — section key in the GET /api/health response object (e.g. "mqtt").
// get          — called at GET time; writes fields into the provided child bb_json_t.
// ctx          — opaque context pointer passed to get.
// schema_props — complete JSON-Schema value for this section's object
//                (e.g. '{"type":"object","properties":{...}}').
//                Must have static/rodata lifetime. NULL → no schema contribution.
//
// Returns BB_ERR_INVALID_ARG if name or get is NULL.
// Returns BB_ERR_INVALID_STATE if called after the registry is frozen (server started).
// Returns BB_ERR_NO_SPACE if the section table is full.
bb_err_t bb_health_register_section(const char *name,
                                     bb_response_get_fn get,
                                     void *ctx,
                                     const char *schema_props);

#ifdef ESP_PLATFORM
#include "bb_http_server.h"

// ---------------------------------------------------------------------------
// Registry hooks
// ---------------------------------------------------------------------------

// Reserve route-table slots for bb_health before the HTTP server starts.
// bbtool:init tier=pre_http fn=bb_health_reserve_routes
bb_err_t bb_health_reserve_routes(void);

// Registry hook — registers GET /api/health and starts the stack
// high-water monitor.
// bbtool:init tier=regular fn=bb_health_init server=true
bb_err_t bb_health_init(bb_http_handle_t server);

// Registry hook — registers the low-stack transition handler with
// bb_task_registry. Gated behind CONFIG_BB_HEALTH_STACK_AUTOSTART (feature
// toggle, not registration glue — KEPT).
// bbtool:init tier=pre_http fn=bb_health_stack_monitor_start
bb_err_t bb_health_stack_monitor_start(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
