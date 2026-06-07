// bb_power_routes — GET /api/power route + route-extender bearer for bb_power.
//
// Consumers register this component (REQUIRES bb_power_routes) to expose
// /api/power. Satellite components register power-route extenders via:
//
//   bb_http_register_route_extender("power", my_fn, my_schema_frag);
//
// before bb_http_extender_freeze() is called (regular-tier init, order 0).
// bb_power_routes_init assembles the schema after freeze (order 1).
//
// Route-extender ordering (same constraint as bb_info):
//   order 0 — satellites register their "power" extenders
//   order 1 — bb_power_routes_init freezes + assembles + registers /api/power
//
// Host twin: platform/host/bb_power_routes/bb_power_routes_host.c
#pragma once
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register GET /api/power with the HTTP server (regular-tier init fn).
// Assembles the route schema incorporating any registered extenders.
// Must be called after all "power" extenders have been registered.
bb_err_t bb_power_routes_init(bb_http_handle_t server);

#ifdef BB_POWER_ROUTES_TESTING

#include "bb_power_test.h"
#include "bb_http_extender_test.h"

// Assemble (or return cached) schema for the "power" route_id.
// Caller must NOT free the result.
const char *bb_power_routes_get_assembled_schema(void);

// Reset route state for test isolation.
void bb_power_routes_reset_for_test(void);

#endif /* BB_POWER_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
