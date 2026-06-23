#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_json.h"
#include <stdbool.h>
#include <string.h>

// Host twin of the bb_fan_routes component — emit helper + testing hooks.
// /api/fan route deleted in B1-269 PR7; bb_fan_emit_section remains for
// /api/sensors fan section (SSOT).

#ifdef CONFIG_BB_FAN_AUTOFAN
static void (*s_persist_cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg) = NULL;
static void *s_persist_ctx = NULL;

void bb_fan_routes_set_autofan_persist_cb(
    void (*cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg), void *ctx)
{
    s_persist_cb  = cb;
    s_persist_ctx = ctx;
}

void bb_fan_routes_invoke_persist_cb(const bb_fan_autofan_cfg_t *cfg)
{
    if (s_persist_cb) {
        s_persist_cb(s_persist_ctx, cfg);
    }
}
#endif /* CONFIG_BB_FAN_AUTOFAN */

// ---------------------------------------------------------------------------
// Shared emit helper — host implementation mirrors espidf (SSOT).
// ---------------------------------------------------------------------------

void bb_fan_emit_section(bb_json_t obj)
{
    bb_fan_handle_t h = bb_fan_primary();
    bool present = (h != NULL);
    bb_fan_snapshot_t snap;
    bb_fan_snapshot(h, &snap);
    bb_json_obj_set_bool(obj, "present", present);
#ifndef CONFIG_BB_FAN_AUTOFAN
    bb_fan_emit(obj, &snap);
#else
    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);
    bb_fan_emit(obj, &snap, &tel);
#endif
}

#ifdef BB_FAN_ROUTES_TESTING

void bb_fan_routes_reset_for_test(void)
{
    bb_fan_test_reset();
#ifdef CONFIG_BB_FAN_AUTOFAN
    s_persist_cb  = NULL;
    s_persist_ctx = NULL;
#endif
}

#endif /* BB_FAN_ROUTES_TESTING */
