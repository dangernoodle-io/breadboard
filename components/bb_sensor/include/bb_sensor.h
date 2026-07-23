// bb_sensor — HTTP-free cross-HAL sensor collector: per-domain snapshot
// getters (fan/power/thermal) plus the fan write path. Portable single TU
// (host + ESP-IDF, no platform split) -- absorbs bb_thermal_collect() (the
// deleted bb_thermal component) and the HAL-read/HAL-write halves of
// bb_sensor_http_wire.c's fan/power/thermal gather + fan apply.
//
// bb_sensor_http (components/bb_sensor_http) is the sole consumer: it binds
// these onto /api/sensors/{fan,power,thermal} and owns ONLY the wire
// (de)serialization + HTTP status mapping -- no HAL calls of its own.
#pragma once
#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Fan (CONFIG resource -- compile-forked identically to bb_sensor_http's
// wire struct: CONFIG_BB_FAN_AUTOFAN selects the autofan-cfg shape, its
// absence selects the flat manual duty_pct shape). The SAME struct serves
// both the read path (bb_sensor_fan_snapshot) and the write path
// (bb_sensor_fan_apply) -- GET/PATCH round-trip the same fields.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_FAN_AUTOFAN
typedef struct {
    bool  present;       // true if a primary fan handle is bound
    bool  autofan;       // true = PID mode; false = manual_pct (bb_fan_autofan_cfg_t.enabled)
    float die_target_c;  // ASIC die PID setpoint (°C)
    float aux_target_c;  // Aux/VR PID setpoint (°C); bb_fan's own field name --
                          // bb_sensor_http renames this to "vr_target_c" at the wire boundary
    int   manual_pct;    // Duty when !autofan [0..100]
    int   min_pct;       // Lower output limit [0..100]
} bb_sensor_fan_snapshot_t;
#else
typedef struct {
    bool present;   // true if a primary fan handle is bound
    int  duty_pct;  // fixed sentinel -1 -- no "current duty" concept is exposed
                    // in non-autofan mode (mirrors the old fan_section_patch contract)
} bb_sensor_fan_snapshot_t;
#endif

// Read the current fan config (autofan cfg, or the fixed present-only
// sentinel shape in non-autofan mode) from the primary fan handle. Does NOT
// poll (callers must have already polled bb_fan_poll()). Always succeeds --
// no primary fan is an ordinary hardware state, reported via
// present == false, not a failure.
void bb_sensor_fan_snapshot(bb_sensor_fan_snapshot_t *out);

// Apply a fan config to the primary fan handle (autofan set + validation,
// or manual duty_pct set, per CONFIG_BB_FAN_AUTOFAN).
//
// Returns BB_ERR_UNSUPPORTED if no primary fan is bound -- an ordinary,
// possibly-transient no-hardware condition, distinct from the capability
// gap below.
// Returns BB_ERR_VALIDATION if any field is out of range (autofan mode only).
// Returns BB_ERR_INVALID_STATE if a primary fan IS bound but its driver
// lacks the needed vtable slot (set_duty_pct) -- a nullable capability gap
// (non-autofan mode only), deliberately retargeted from
// bb_fan_set_duty_pct()'s own BB_ERR_UNSUPPORTED so it stays distinguishable
// from the no-primary-fan case above (both would otherwise collide on the
// same error value).
bb_err_t bb_sensor_fan_apply(const bb_sensor_fan_snapshot_t *cfg);

// ---------------------------------------------------------------------------
// Power (read-only telemetry -- flat, mirrors bb_power_snapshot_t plus a
// present flag for "no primary power handle bound")
// ---------------------------------------------------------------------------
typedef struct {
    bool present;
    int  vout_mv;
    int  iout_ma;
    int  pout_mw;
    int  vin_mv;
    int  temp_c;
} bb_sensor_power_snapshot_t;

// Read the primary power handle's cached snapshot. Does NOT poll (callers
// must have already polled bb_power_poll()). If no primary power handle is
// bound, present=false and all fields are -1 (bb_power_snapshot()'s own
// NULL-handle contract).
void bb_sensor_power_snapshot(bb_sensor_power_snapshot_t *out);

// ---------------------------------------------------------------------------
// Thermal (read-only telemetry -- absorbed from the deleted bb_thermal
// component; SSOT for which HAL each temperature comes from)
// ---------------------------------------------------------------------------

// present=false means the hardware is absent or reading failed; the
// paired _c field is undefined when present=false. vr_hw_present /
// fan_hw_present distinguish "no hardware" from "hardware present but no
// reading".
typedef struct {
    bool  soc_present;      // true if SoC temp readable
    float soc_c;            // valid when soc_present

    bool  vr_hw_present;    // true if power primary != NULL
    bool  vr_present;       // true if vr_hw_present AND reading valid (temp_c >= 0)
    float vr_c;             // valid when vr_present

    bool  fan_hw_present;   // true if fan primary != NULL
    bool  asic_present;     // true if fan_hw_present AND die_c is not NaN
    float asic_c;           // valid when asic_present
    bool  board_present;    // true if fan_hw_present AND board_c is not NaN
    float board_c;          // valid when board_present
} bb_sensor_thermal_snapshot_t;

// Read a full thermal snapshot across bb_temp (SoC) / bb_power (VR) /
// bb_fan (ASIC die + board). Does NOT poll (callers must have already
// polled bb_power_poll()/bb_fan_poll()).
void bb_sensor_thermal_snapshot(bb_sensor_thermal_snapshot_t *out);

#ifdef __cplusplus
}
#endif
