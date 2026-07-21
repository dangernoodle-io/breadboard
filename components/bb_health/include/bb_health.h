#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "bb_core.h"

// Compute the /api/health.ok gate: true when WiFi has IP. ota_validated
// dropped (B1-977, bb_board dissolution). mDNS is intentionally excluded
// (locked decision B1-269).
bool bb_health_compute_ok(void);

// Register a named section for /api/health: see bb_health_section_register()
// (bb_health_section.h) -- the composer-shaped registry the live handler
// (platform/espidf/bb_health/bb_health.c) composes its document from
// (B1-1100). The legacy bb_response_registry_t-backed
// bb_health_register_section() this header used to declare is retired.

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
