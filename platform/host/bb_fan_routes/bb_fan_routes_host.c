#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_http_extender.h"
#include "bb_json.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

// Host twin of the /api/fan route — provides schema assembly and testing hooks.

#ifdef CONFIG_BB_FAN_AUTOFAN
// Persist callback (shared state — mirrors espidf implementation).
static void (*s_persist_cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg) = NULL;
static void *s_persist_ctx = NULL;

void bb_fan_routes_set_autofan_persist_cb(
    void (*cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg), void *ctx)
{
    s_persist_cb  = cb;
    s_persist_ctx = ctx;
}

// Called by the host POST handler (in test_bb_fan_routes.c) after bb_fan_set_autofan().
void bb_fan_routes_invoke_persist_cb(const bb_fan_autofan_cfg_t *cfg)
{
    if (s_persist_cb) {
        s_persist_cb(s_persist_ctx, cfg);
    }
}
#endif /* CONFIG_BB_FAN_AUTOFAN */

#ifdef CONFIG_BB_FAN_AUTOFAN
static const char k_fan_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]},"
    "\"autofan\":{\"type\":\"boolean\",\"description\":\"autofan enabled\"},"
    "\"die_target_c\":{\"type\":\"number\",\"description\":\"ASIC die target temperature\"},"
    "\"vr_target_c\":{\"type\":\"number\",\"description\":\"VR target temperature\"},"
    "\"manual_pct\":{\"type\":\"integer\",\"description\":\"manual duty % when autofan disabled\"},"
    "\"min_pct\":{\"type\":\"integer\",\"description\":\"minimum fan duty %\"},"
    "\"die_ema_c\":{\"type\":[\"number\",\"null\"],\"description\":\"filtered ASIC die temperature\"},"
    "\"vr_ema_c\":{\"type\":[\"number\",\"null\"],\"description\":\"filtered VR temperature\"},"
    "\"pid_input_c\":{\"type\":[\"number\",\"null\"],\"description\":\"PID input selected by max(err/target) ratio\"},"
    "\"pid_input_src\":{\"type\":\"string\",\"enum\":[\"die\",\"vr\"],\"description\":\"which sensor is driving PID: die or vr\"}";
#else
static const char k_fan_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]}";
#endif

static const char k_fan_schema_suffix[] =
    "},"
    "\"required\":[\"present\"]}";

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

#ifdef BB_FAN_ROUTES_TESTING

const char *bb_fan_routes_get_assembled_schema(void)
{
    const char *cached = bb_http_extender_get_assembled_schema("fan");
    if (cached) return cached;
    return bb_http_route_assemble_schema("fan", k_fan_schema_base, k_fan_schema_suffix);
}

void bb_fan_routes_reset_for_test(void)
{
    bb_fan_test_reset();
#ifdef CONFIG_BB_FAN_AUTOFAN
    s_persist_cb  = NULL;
    s_persist_ctx = NULL;
#endif
    // extender reset is handled globally by bb_info_reset_for_test → bb_http_extender_reset_for_test
}

#endif /* BB_FAN_ROUTES_TESTING */
