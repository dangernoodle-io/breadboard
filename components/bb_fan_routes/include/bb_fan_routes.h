// bb_fan_routes — emit helper for bb_fan data (SSOT for /api/sensors fan section).
//
// NOTE: GET /api/fan and POST /api/fan routes were deleted in B1-269 PR7.
// /api/sensors (bb_sensors) is the primary HTTP surface for fan data.
// bb_fan_routes_init() is a no-op stub kept for link compatibility.
//
// Host twin: platform/host/bb_fan_routes/bb_fan_routes_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// No-op stub kept for link compatibility. /api/fan route deleted in B1-269 PR7.
bb_err_t bb_fan_routes_init(bb_http_handle_t server);

// Shared emit helper — writes fan fields into an existing bb_json_t object.
// Called by /api/sensors fan section get_fn (SSOT, no behavior drift).
// Takes a bb_json_t obj — the caller owns it and must set it on the parent.
void bb_fan_emit_section(bb_json_t obj);

#ifdef CONFIG_BB_FAN_AUTOFAN
#include "bb_fan.h"

// Register a callback invoked AFTER a fan control operation via the HTTP path.
// Pass NULL for cb to clear the callback.
// Not thread-safe against concurrent requests; call once at init.
void bb_fan_routes_set_autofan_persist_cb(
    void (*cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg), void *ctx);
#endif /* CONFIG_BB_FAN_AUTOFAN */

#ifdef BB_FAN_ROUTES_TESTING

#include "bb_fan_test.h"

// Reset route state for test isolation.
void bb_fan_routes_reset_for_test(void);

#ifdef CONFIG_BB_FAN_AUTOFAN
// Invoke the registered persist callback (used by host test handlers).
void bb_fan_routes_invoke_persist_cb(const bb_fan_autofan_cfg_t *cfg);
#endif

#endif /* BB_FAN_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
