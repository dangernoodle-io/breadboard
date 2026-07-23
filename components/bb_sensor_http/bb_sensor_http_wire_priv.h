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

// ---------------------------------------------------------------------------
// JSON Schema (B1-1180 PR-2) -- hand-authored, on-device (not host-gated;
// same posture as every other exemplar's SSOT schema literal), co-located
// with the descriptors above in bb_sensor_http_wire.c. Fidelity against the
// BB_SERIALIZE_META_HOST-gated meta tables below is proven by
// test_bb_sensor_http_wire_meta_golden.c.
// ---------------------------------------------------------------------------
extern const char *const bb_sensor_http_fan_schema;          // GET /api/sensors/fan response
extern const char *const bb_sensor_http_fan_request_schema;  // PATCH /api/sensors/fan request
extern const char *const bb_sensor_http_power_schema;        // GET /api/sensors/power response
extern const char *const bb_sensor_http_thermal_schema;      // GET /api/sensors/thermal response

#ifdef ESP_PLATFORM
// Registers PRODUCER-OWNED `static const` describe-only bb_route_t entries
// (handler=NULL, .rodata/flash, never DRAM) for GET+PATCH /api/sensors/fan,
// GET /api/sensors/power, and GET /api/sensors/thermal via
// bb_http_register_route_descriptor_only() -- makes these paths VISIBLE to
// bb_openapi_emit() without touching the live GET+PATCH /api/sensors/*
// wildcard dispatch registered by bb_sensor_http_bind_and_register()/
// bb_http_section_init(). Called once from bb_sensor_http_init()
// (platform/espidf/bb_sensor_http/bb_sensor_http.c), after
// bb_http_section_init(). power/thermal are GET-only by construction here --
// their bb_data bindings have no apply hook (PATCH already 405s at the
// dispatch layer), so no PATCH describe route is registered for either.
bb_err_t bb_sensor_http_describe_routes(void);
#endif /* ESP_PLATFORM */

#if defined(BB_SERIALIZE_META_HOST)
#include "bb_serialize_meta.h"

// Fan meta/golden layer (B1-1180 PR-2 review fix, HIGH 2 restructure):
//
//   - `bb_sensor_http_fan_meta` / `bb_sensor_http_fan_request_meta` are
//     PRODUCTION aliases -- #ifdef CONFIG_BB_FAN_AUTOFAN-selected, same
//     fork as the real `bb_sensor_http_fan_wire_desc` above, so pairing
//     them with that real descriptor golden-tests PRODUCTION (not a twin)
//     for whichever variant this build's Kconfig actually compiles.
//   - `bb_sensor_http_fan_{autofan,manual}_shape_desc` +
//     `bb_sensor_http_fan_{autofan,manual}_meta` + `_schema` are
//     self-contained TWINS (dummy zero `.offset` fields -- unused by the
//     schema composer/validator, see bb_sensor_http_wire.c's doc comment)
//     covering ONLY the schema shape, independent of the real production
//     descriptor. test_bb_sensor_http_wire_meta_golden.c exercises the twin
//     ONLY for whichever variant is currently INACTIVE (dark-branch
//     coverage, B1-1093) -- the active variant already gets real
//     production coverage via the aliases above, so its own twin is never
//     tested (would be redundant, and testing it INSTEAD of production
//     would leave production's own meta table unverified).
extern const bb_serialize_desc_meta_t bb_sensor_http_fan_meta;
extern const bb_serialize_desc_meta_t bb_sensor_http_fan_request_meta;

extern const char *const bb_sensor_http_fan_autofan_schema;
extern const char *const bb_sensor_http_fan_manual_schema;

extern const bb_serialize_desc_t bb_sensor_http_fan_autofan_shape_desc;
extern const bb_serialize_desc_t bb_sensor_http_fan_manual_shape_desc;

extern const bb_serialize_desc_meta_t bb_sensor_http_fan_autofan_meta;
extern const bb_serialize_desc_meta_t bb_sensor_http_fan_manual_meta;

extern const bb_serialize_desc_meta_t bb_sensor_http_power_meta;
extern const bb_serialize_desc_meta_t bb_sensor_http_thermal_meta;
#endif /* BB_SERIALIZE_META_HOST */

#ifdef __cplusplus
}
#endif
