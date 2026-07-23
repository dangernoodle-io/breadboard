#include "bb_sensor.h"

#include "bb_fan.h"
#include "bb_power.h"
#include "bb_temp.h"

#include <math.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Fan
// ---------------------------------------------------------------------------

void bb_sensor_fan_snapshot(bb_sensor_fan_snapshot_t *out)
{
#ifdef CONFIG_BB_FAN_AUTOFAN
    bb_fan_handle_t h = bb_fan_primary();
    out->present = (h != NULL);
    if (!h) {
        out->autofan      = false;
        out->die_target_c = 0.0f;
        out->aux_target_c = 0.0f;
        out->manual_pct   = 0;
        out->min_pct      = 0;
        return;
    }

    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(h, &cfg);
    out->autofan      = cfg.enabled;
    out->die_target_c = cfg.die_target_c;
    out->aux_target_c = cfg.aux_target_c;
    out->manual_pct   = cfg.manual_pct;
    out->min_pct      = cfg.min_pct;
#else
    // Fixed sentinel, deliberately not a real read -- there is no "current
    // duty" concept exposed on this resource in non-autofan mode (mirrors
    // the old fan_section_patch contract). `present` still reflects live
    // hardware state, independent of the sentinel.
    out->present  = (bb_fan_primary() != NULL);
    out->duty_pct = -1;
#endif
}

bb_err_t bb_sensor_fan_apply(const bb_sensor_fan_snapshot_t *cfg)
{
    bb_fan_handle_t h = bb_fan_primary();
    // No primary fan: BB_ERR_UNSUPPORTED (not BB_ERR_INVALID_STATE) -- an
    // ordinary, possibly-transient hardware state, kept distinguishable from
    // the driver capability gap below (see this fn's own header doc).
    if (!h) return BB_ERR_UNSUPPORTED;

#ifdef CONFIG_BB_FAN_AUTOFAN
    if (cfg->die_target_c <= 0.0f)                    return BB_ERR_VALIDATION;
    if (cfg->aux_target_c <= 0.0f)                     return BB_ERR_VALIDATION;
    if (cfg->manual_pct < 0 || cfg->manual_pct > 100)  return BB_ERR_VALIDATION;
    if (cfg->min_pct < 0 || cfg->min_pct > 100)        return BB_ERR_VALIDATION;

    bb_fan_autofan_cfg_t hal_cfg = {
        .enabled      = cfg->autofan,
        .die_target_c = cfg->die_target_c,
        .aux_target_c = cfg->aux_target_c,
        .min_pct      = cfg->min_pct,
        .manual_pct   = cfg->manual_pct,
    };
    return bb_fan_set_autofan(h, &hal_cfg);
#else
    if (cfg->duty_pct < 0 || cfg->duty_pct > 100) return BB_ERR_VALIDATION;

    bb_err_t rc = bb_fan_set_duty_pct(h, cfg->duty_pct);
    // bb_fan_set_duty_pct() has its OWN, independent BB_ERR_UNSUPPORTED --
    // the bound driver's vtable simply has no set_duty_pct (a legitimate,
    // nullable capability gap; see platform/host/bb_fan/bb_fan.c and
    // test/test_host/test_bb_fan.c's drv_minimal coverage). That is a
    // DIFFERENT condition from "no primary fan" above, but shares the same
    // bb_err_t value -- left alone it would collide with the caller's single
    // unsupported-status mapping, masking a real capability gap as "no fan
    // wired". Retarget to BB_ERR_INVALID_STATE so the two conditions stay
    // distinguishable.
    if (rc == BB_ERR_UNSUPPORTED) return BB_ERR_INVALID_STATE;
    return rc;
#endif
}

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

void bb_sensor_power_snapshot(bb_sensor_power_snapshot_t *out)
{
    bb_power_handle_t h = bb_power_primary();
    bb_power_snapshot_t snap;
    bb_power_snapshot(h, &snap);

    out->present = (h != NULL);
    out->vout_mv = snap.vout_mv;
    out->iout_ma = snap.iout_ma;
    out->pout_mw = snap.pout_mw;
    out->vin_mv  = snap.vin_mv;
    out->temp_c  = snap.temp_c;
}

// ---------------------------------------------------------------------------
// Thermal -- absorbed from the deleted bb_thermal component (SSOT for which
// HAL each temperature comes from).
// ---------------------------------------------------------------------------

void bb_sensor_thermal_snapshot(bb_sensor_thermal_snapshot_t *out)
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
// Test hook
// ---------------------------------------------------------------------------

#ifdef BB_SENSOR_TESTING

#include "bb_fan_test.h"
#include "bb_power_test.h"
#include "bb_sensor_test.h"
#include "bb_temp_test.h"

void bb_sensor_reset_for_test(void)
{
    bb_temp_test_set_soc(false, 0.0f);
    bb_fan_test_reset();
    bb_power_test_reset();
}

#endif /* BB_SENSOR_TESTING */
