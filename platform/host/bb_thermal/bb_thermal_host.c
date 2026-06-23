#include "bb_thermal.h"
#include "bb_fan.h"
#include "bb_fan_test.h"
#include "bb_power.h"
#include "bb_power_test.h"
#include "bb_temp.h"
#include "bb_json.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

// Host twin of the bb_thermal component — collect + emit helpers + testing hooks.
// /api/thermal route deleted in B1-269 PR7; bb_thermal_emit_section remains for
// /api/sensors thermal section (SSOT).

// ---------------------------------------------------------------------------
// Internal helper: emit {present, c} sub-object
// ---------------------------------------------------------------------------

static void host_emit_source(bb_json_t root, const char *key, bool present, double c_val)
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
// Pure value collector — SSOT for which HAL each temperature comes from.
// ---------------------------------------------------------------------------

void bb_thermal_collect(bb_thermal_values_t *out)
{
    out->soc_c = 0.0f;
    out->soc_present = bb_temp_read_soc(&out->soc_c);

    bb_power_handle_t pwr = bb_power_primary();
    out->vr_hw_present = (pwr != NULL);
    bb_power_snapshot_t psnap;
    bb_power_snapshot(pwr, &psnap);
    out->vr_present = (out->vr_hw_present && psnap.temp_c >= 0);
    out->vr_c = out->vr_present ? (float)psnap.temp_c : 0.0f;

    bb_fan_handle_t fan = bb_fan_primary();
    out->fan_hw_present = (fan != NULL);
    bb_fan_snapshot_t fsnap;
    bb_fan_snapshot(fan, &fsnap);
    out->asic_present  = (out->fan_hw_present && !isnan(fsnap.die_c));
    out->asic_c        = out->asic_present ? fsnap.die_c : 0.0f;
    out->board_present = (out->fan_hw_present && !isnan(fsnap.board_c));
    out->board_c       = out->board_present ? fsnap.board_c : 0.0f;
}

// ---------------------------------------------------------------------------
// Shared emit helper — host implementation mirrors espidf (SSOT).
// ---------------------------------------------------------------------------

void bb_thermal_emit_section(bb_json_t obj)
{
    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    host_emit_source(obj, "soc",   v.soc_present,   v.soc_present   ? (double)v.soc_c   : 0.0);
    host_emit_source(obj, "vr",    v.vr_present,    v.vr_present    ? (double)v.vr_c    : 0.0);
    host_emit_source(obj, "asic",  v.asic_present,  v.asic_present  ? (double)v.asic_c  : 0.0);
    host_emit_source(obj, "board", v.board_present, v.board_present ? (double)v.board_c : 0.0);
}

#ifdef BB_THERMAL_TESTING

void bb_thermal_reset_for_test(void)
{
    bb_temp_test_set_soc(false, 0.0f);
    bb_fan_test_reset();
    bb_power_test_reset();
}

#endif /* BB_THERMAL_TESTING */
