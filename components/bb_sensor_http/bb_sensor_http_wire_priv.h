#pragma once

// Private: bb_data wire descriptors for the /api/sensors/* per-section
// dispatch (bb_sensor_http PR-2, B1-828 epic). FULL BREAK of the old composite
// GET/PATCH /api/sensors endpoint -- zero live consumers workspace-wide, so
// no back-compat shim. See bb_http_section.h for the registry-agnostic
// dispatch contract these bindings serve against.
//
// "fan" is a CONFIG resource (autofan cfg, or manual duty when autofan is
// not compiled in) -- GET/PATCH round-trip the SAME fields
// bb_fan_set_autofan()/bb_fan_set_duty_pct() consume, not fan telemetry
// (rpm/die_c/board_c -- those are no longer served here). "power"/"thermal"
// are read-only telemetry snapshots (gather-only bindings; apply == NULL ->
// bb_http_section maps PATCH to 405).
//
// Compiled on both host and ESP-IDF -- bb_sensor_{fan,power,thermal}_snapshot()
// and bb_sensor_fan_apply() (components/bb_sensor) are all portable; this
// layer does no HAL calls of its own, only wire (de)serialization.
#include "bb_core.h"
#include "bb_data.h"
#include "bb_serialize.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Fan (CONFIG resource -- compile-forked like the old fan_section_patch)
// ---------------------------------------------------------------------------

// `present` reflects whether a primary fan exists at gather time -- GET
// always succeeds (200), reporting absence via `present == false` rather
// than failing, in BOTH variants below (bb_sensor_http_fan_gather()'s own doc).
#ifdef CONFIG_BB_FAN_AUTOFAN
typedef struct {
    bool    present;
    bool    autofan;
    double  die_target_c;
    double  vr_target_c;   // wire name for bb_fan_autofan_cfg_t.aux_target_c
    int64_t manual_pct;
    int64_t min_pct;
} bb_sensor_http_fan_wire_t;
#else
typedef struct {
    bool    present;
    int64_t duty_pct;
} bb_sensor_http_fan_wire_t;
#endif

// ---------------------------------------------------------------------------
// Power (read-only telemetry -- flat, -1-sentinel present-gated per field)
// ---------------------------------------------------------------------------

typedef struct {
    bool    present;
    int64_t vout_mv;
    int64_t iout_ma;
    int64_t pout_mw;
    int64_t vin_mv;
    int64_t temp_c;
} bb_sensor_http_power_wire_t;

// ---------------------------------------------------------------------------
// Thermal (read-only telemetry -- nested {present,c} per source, matches
// bb_thermal_collect()'s own field set)
// ---------------------------------------------------------------------------

typedef struct {
    bool   present;
    double c;
} bb_sensor_http_thermal_source_wire_t;

typedef struct {
    bb_sensor_http_thermal_source_wire_t soc;
    bb_sensor_http_thermal_source_wire_t vr;
    bb_sensor_http_thermal_source_wire_t asic;
    bb_sensor_http_thermal_source_wire_t board;
} bb_sensor_http_thermal_wire_t;

extern const bb_serialize_desc_t bb_sensor_http_fan_wire_desc;
extern const bb_serialize_desc_t bb_sensor_http_power_wire_desc;
extern const bb_serialize_desc_t bb_sensor_http_thermal_wire_desc;

// Gather hooks (bb_data_gather_fn-shaped) -- bound to keys "fan"/"power"/
// "thermal" in bb_sensor_http_bind_and_register() (bb_sensor_http_dispatch.c).
bb_err_t bb_sensor_http_fan_gather(void *dst, const bb_data_gather_args_t *args);
bb_err_t bb_sensor_http_power_gather(void *dst, const bb_data_gather_args_t *args);
bb_err_t bb_sensor_http_thermal_gather(void *dst, const bb_data_gather_args_t *args);

// Apply hook (bb_data_apply_fn-shaped) -- fan only. power/thermal are
// gather-only (egress-only) bindings, no apply hook bound.
bb_err_t bb_sensor_http_fan_apply(const void *snap, const bb_data_apply_args_t *args);

#ifdef __cplusplus
}
#endif
