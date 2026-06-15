// bb_thermal — emit helper for aggregate thermal data (SSOT for /api/sensors thermal section).
//
// NOTE: GET /api/thermal route was deleted in B1-269 PR7.
// /api/sensors (bb_sensors) is the primary HTTP surface for thermal data.
// bb_thermal_init() is a no-op stub kept for link compatibility.
//
// Host twin: platform/host/bb_thermal/bb_thermal_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// No-op stub kept for link compatibility. /api/thermal route deleted in B1-269 PR7.
bb_err_t bb_thermal_init(bb_http_handle_t server);

// Shared emit helper — writes thermal sub-objects into an existing bb_json_t.
// Emits {soc,vr,asic,board} each as {present,c|null} (nested shape).
// Called by /api/sensors thermal section get_fn (SSOT).
void bb_thermal_emit_section(bb_json_t obj);

#ifdef BB_THERMAL_TESTING

#include "bb_temp_test.h"

// Reset state for test isolation.
void bb_thermal_reset_for_test(void);

#endif /* BB_THERMAL_TESTING */

#ifdef __cplusplus
}
#endif
