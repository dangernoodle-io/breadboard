// bb_thermal — aggregate thermal route: GET /api/thermal (soc/vr/asic/board).
//
// Aggregates temperatures from bb_temp (SoC), bb_power (VR), and bb_fan
// (ASIC die + board) HALs into a single /api/thermal endpoint.
//
// Consumer wanting the aggregate thermal view adds:
//   REQUIRES bb_thermal
//
// Route-extender ordering (same constraint as bb_info):
//   order 0 — satellites register their "thermal" extenders
//   order 1 — bb_thermal_init freezes + assembles + registers /api/thermal
//
// Host twin: platform/host/bb_thermal/bb_thermal_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register GET /api/thermal with the HTTP server (regular-tier init fn).
// Assembles the route schema incorporating any registered extenders.
// Must be called after all "thermal" extenders have been registered.
bb_err_t bb_thermal_init(bb_http_handle_t server);

// Shared emit helper — writes thermal sub-objects into an existing bb_json_t.
// Emits {soc,vr,asic,board} each as {present,c|null} (route's nested shape;
// intentionally different from bb_pub_thermal's flat MQTT shape).
// Called by both /api/thermal GET handler and /api/sensors thermal section get_fn.
void bb_thermal_emit_section(bb_json_t obj);

#ifdef BB_THERMAL_TESTING

#include "bb_temp_test.h"
#include "bb_http_extender_test.h"

// Assemble (or return cached) schema for the "thermal" route_id.
// Caller must NOT free the result.
const char *bb_thermal_get_assembled_schema(void);

// Reset route state for test isolation.
void bb_thermal_reset_for_test(void);

#endif /* BB_THERMAL_TESTING */

#ifdef __cplusplus
}
#endif
