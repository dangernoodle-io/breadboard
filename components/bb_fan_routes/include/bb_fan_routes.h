// bb_fan_routes — GET /api/fan + POST /api/fan route + route-extender bearer for bb_fan.
//
// Consumers register this component (REQUIRES bb_fan_routes) to expose /api/fan.
// Satellite components register fan-route extenders via:
//
//   bb_http_register_route_extender("fan", my_fn, my_schema_frag);
//
// before bb_http_extender_freeze() is called (regular-tier init, order 0).
// bb_fan_routes_init assembles the schema after freeze (order 1).
//
// Route-extender ordering (same constraint as bb_info):
//   order 0 — satellites register their "fan" extenders
//   order 1 — bb_fan_routes_init freezes + assembles + registers /api/fan
//
// NOTE: POST /api/fan here provides generic raw-duty control (sets duty_pct on
// the primary fan handle). TM's existing POST /api/fan sets autofan config;
// that reconciliation is deferred to P4 — this BB route is the generic HAL
// control surface.
//
// Host twin: platform/host/bb_fan_routes/bb_fan_routes_host.c
#pragma once
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register GET+POST /api/fan with the HTTP server (regular-tier init fn).
// Assembles the route schema incorporating any registered extenders.
// Must be called after all "fan" extenders have been registered.
bb_err_t bb_fan_routes_init(bb_http_handle_t server);

#ifdef BB_FAN_ROUTES_TESTING

#include "bb_fan_test.h"
#include "bb_http_extender_test.h"

// Assemble (or return cached) schema for the "fan" route_id.
// Caller must NOT free the result.
const char *bb_fan_routes_get_assembled_schema(void);

// Reset route state for test isolation.
void bb_fan_routes_reset_for_test(void);

#endif /* BB_FAN_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
