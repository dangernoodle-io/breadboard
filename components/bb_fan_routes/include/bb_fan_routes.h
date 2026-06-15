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
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register GET+POST /api/fan with the HTTP server (regular-tier init fn).
// Assembles the route schema incorporating any registered extenders.
// Must be called after all "fan" extenders have been registered.
bb_err_t bb_fan_routes_init(bb_http_handle_t server);

// Shared emit helper — writes fan fields into an existing bb_json_t object.
// Called by both /api/fan GET handler and /api/sensors fan section get_fn so
// both routes share one emitter (SSOT, no behavior drift).
// Takes a bb_json_t obj — the caller owns it and must set it on the parent.
void bb_fan_emit_section(bb_json_t obj);

#ifdef CONFIG_BB_FAN_AUTOFAN
#include "bb_fan.h"

// Register a callback invoked AFTER a POST /api/fan-driven autofan config change
// is successfully applied. The callback fires only from the POST handler path —
// NOT from direct bb_fan_set_autofan() calls — so boot-time NVS loads do not
// trigger a redundant re-persist. Pass NULL for cb to clear the callback.
// Not thread-safe against concurrent POST requests; call once at init.
void bb_fan_routes_set_autofan_persist_cb(
    void (*cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg), void *ctx);
#endif /* CONFIG_BB_FAN_AUTOFAN */

#ifdef BB_FAN_ROUTES_TESTING

#include "bb_fan_test.h"
#include "bb_http_extender_test.h"

// Assemble (or return cached) schema for the "fan" route_id.
// Caller must NOT free the result.
const char *bb_fan_routes_get_assembled_schema(void);

// Reset route state for test isolation.
void bb_fan_routes_reset_for_test(void);

#ifdef CONFIG_BB_FAN_AUTOFAN
// Invoke the registered persist callback (used by host test POST handler).
void bb_fan_routes_invoke_persist_cb(const bb_fan_autofan_cfg_t *cfg);
#endif

#endif /* BB_FAN_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
