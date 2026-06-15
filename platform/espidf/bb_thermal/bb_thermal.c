#include "bb_thermal.h"
#include "bb_fan.h"
#include "bb_power.h"
#include "bb_temp.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

static const char *TAG __attribute__((unused)) = "bb_thermal";

// ---------------------------------------------------------------------------
// Emit helper — builds a {present, c} sub-object
// ---------------------------------------------------------------------------

static void emit_source(bb_json_t root, const char *key, bool present, double c_val)
{
    bb_json_t obj = bb_json_obj_new();
    if (!obj) return;  // OOM: skip sub-object; do not set NULL key on root
    bb_json_obj_set_bool(obj, "present", present);
    if (present) {
        bb_json_obj_set_number(obj, "c", c_val);
    } else {
        bb_json_obj_set_null(obj, "c");
    }
    bb_json_obj_set_obj(root, key, obj);
}

// ---------------------------------------------------------------------------
// Shared emit helper — writes thermal sub-objects into an existing bb_json_t.
// Emits {soc,vr,asic,board} each as {present,c|null} (route's nested shape).
// Used by /api/sensors thermal section (SSOT).
// ---------------------------------------------------------------------------

void bb_thermal_emit_section(bb_json_t obj)
{
    float soc_c = 0.0f;
    bool soc_present = bb_temp_read_soc(&soc_c);

    bb_power_handle_t pwr = bb_power_primary();
    bb_power_snapshot_t psnap;
    bb_power_snapshot(pwr, &psnap);
    bool vr_present = (pwr != NULL && psnap.temp_c >= 0);

    bb_fan_handle_t fan = bb_fan_primary();
    bb_fan_snapshot_t fsnap;
    bb_fan_snapshot(fan, &fsnap);
    bool asic_present  = (fan != NULL && !isnan(fsnap.die_c));
    bool board_present = (fan != NULL && !isnan(fsnap.board_c));

    emit_source(obj, "soc",   soc_present,  soc_present   ? (double)soc_c        : 0.0);
    emit_source(obj, "vr",    vr_present,   vr_present    ? (double)psnap.temp_c  : 0.0);
    emit_source(obj, "asic",  asic_present, asic_present  ? (double)fsnap.die_c   : 0.0);
    emit_source(obj, "board", board_present,board_present ? (double)fsnap.board_c  : 0.0);
}

// ---------------------------------------------------------------------------
// bb_thermal_init — stub; /api/thermal route deleted in B1-269 PR7.
// Kept so consumers that previously called this still link.
// ---------------------------------------------------------------------------

bb_err_t bb_thermal_init(bb_http_handle_t server)
{
    (void)server;
    return BB_OK;
}

#if CONFIG_BB_THERMAL_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_thermal, bb_thermal_init, 1);
#endif
