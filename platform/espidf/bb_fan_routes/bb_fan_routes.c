#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

static const char *TAG __attribute__((unused)) = "bb_fan_routes";

#ifdef CONFIG_BB_FAN_AUTOFAN
// Persist callback registered by the consumer (e.g. TM) to save config to NVS.
// NOTE: POST /api/fan is deleted; this callback is kept for consumers that call
// bb_fan_set_autofan directly and want the persist hook.
static void (*s_persist_cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg) = NULL;
static void *s_persist_ctx = NULL;

void bb_fan_routes_set_autofan_persist_cb(
    void (*cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg), void *ctx)
{
    s_persist_cb  = cb;
    s_persist_ctx = ctx;
}
#endif /* CONFIG_BB_FAN_AUTOFAN */

// ---------------------------------------------------------------------------
// Shared emit helper — writes fan fields into an existing bb_json_t object.
// Used by /api/sensors fan section (SSOT).
// ---------------------------------------------------------------------------

void bb_fan_emit_section(bb_json_t obj)
{
    bb_fan_handle_t h = bb_fan_primary();
    bool present = (h != NULL);

    bb_fan_snapshot_t snap;
    bb_fan_snapshot(h, &snap);

    bb_json_obj_set_bool(obj, "present", present);

    if (present && snap.rpm >= 0) {
        bb_json_obj_set_number(obj, "rpm", (double)snap.rpm);
    } else {
        bb_json_obj_set_null(obj, "rpm");
    }

    if (present && snap.duty_pct >= 0) {
        bb_json_obj_set_number(obj, "duty_pct", (double)snap.duty_pct);
    } else {
        bb_json_obj_set_null(obj, "duty_pct");
    }

#ifdef CONFIG_BB_FAN_AUTOFAN
    if (present && h) {
        bb_fan_autofan_cfg_t cfg;
        bb_fan_get_autofan_cfg(h, &cfg);
        bb_json_obj_set_bool(obj,   "autofan",      cfg.enabled);
        bb_json_obj_set_number(obj, "die_target_c", (double)cfg.die_target_c);
        bb_json_obj_set_number(obj, "vr_target_c",  (double)cfg.aux_target_c);
        bb_json_obj_set_number(obj, "manual_pct",   (double)cfg.manual_pct);
        bb_json_obj_set_number(obj, "min_pct",      (double)cfg.min_pct);

        bb_fan_autofan_telemetry_t tel;
        bb_fan_get_autofan_telemetry(h, &tel);

        if (tel.die_ema_c >= 0.0f) {
            bb_json_obj_set_number(obj, "die_ema_c", (double)tel.die_ema_c);
        } else {
            bb_json_obj_set_null(obj, "die_ema_c");
        }
        if (tel.aux_ema_c >= 0.0f) {
            bb_json_obj_set_number(obj, "vr_ema_c", (double)tel.aux_ema_c);
        } else {
            bb_json_obj_set_null(obj, "vr_ema_c");
        }
        if (tel.pid_input_c >= 0.0f) {
            bb_json_obj_set_number(obj, "pid_input_c", (double)tel.pid_input_c);
        } else {
            bb_json_obj_set_null(obj, "pid_input_c");
        }
        const char *src = tel.pid_input_src ? tel.pid_input_src : "";
        if (src[0] == 'a') src = "vr";
        bb_json_obj_set_string(obj, "pid_input_src", src);
    }
#endif /* CONFIG_BB_FAN_AUTOFAN */
}

// ---------------------------------------------------------------------------
// bb_fan_routes_init — stub; /api/fan route deleted in B1-269 PR7.
// Kept so consumers that previously called this still link.
// ---------------------------------------------------------------------------

bb_err_t bb_fan_routes_init(bb_http_handle_t server)
{
    (void)server;
    return BB_OK;
}

#if CONFIG_BB_FAN_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_fan_routes, bb_fan_routes_init, 1);
#endif
