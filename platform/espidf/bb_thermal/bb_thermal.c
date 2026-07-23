#include "bb_thermal.h"
#include "bb_fan.h"
#include "bb_power.h"
#include "bb_temp.h"
#include "bb_log.h"
#include "bb_http_server.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

static const char *TAG __attribute__((unused)) = "bb_thermal";

// ---------------------------------------------------------------------------
// Pure value collector — SSOT for which HAL each temperature comes from.
// bb_thermal_collect() is now consumed by bb_sensor_http_thermal_gather()
// (components/bb_sensor_http/bb_sensor_http_wire.c) for /api/sensors/thermal
// (B1-828 PR-2); the old bb_thermal_emit_section() JSON-emit wrapper (the
// composite /api/sensors endpoint's SSOT) was deleted with it -- FULL
// BREAK, no other caller.
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
// bb_thermal_init — stub; /api/thermal route deleted in B1-269 PR7.
// Kept so consumers that previously called this still link.
// ---------------------------------------------------------------------------

bb_err_t bb_thermal_init(bb_http_handle_t server)
{
    (void)server;
    return BB_OK;
}
