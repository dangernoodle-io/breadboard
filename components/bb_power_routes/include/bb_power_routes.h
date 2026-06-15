// bb_power_routes — emit helper for bb_power data (SSOT for /api/sensors power section).
//
// NOTE: GET /api/power route was deleted in B1-269 PR7.
// /api/sensors (bb_sensors) is the primary HTTP surface for power data.
// bb_power_routes_init() is a no-op stub kept for link compatibility.
//
// Host twin: platform/host/bb_power_routes/bb_power_routes_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// No-op stub kept for link compatibility. /api/power route deleted in B1-269 PR7.
bb_err_t bb_power_routes_init(bb_http_handle_t server);

// Shared emit helper — writes power fields into an existing bb_json_t object.
// Called by /api/sensors power section get_fn so both share one emitter (SSOT).
void bb_power_emit_section(bb_json_t obj);

#ifdef BB_POWER_ROUTES_TESTING

#include "bb_power_test.h"

// Reset route state for test isolation.
void bb_power_routes_reset_for_test(void);

#endif /* BB_POWER_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
