// bb_sensor_http_wire -- the bb_data descriptors + gather/apply hooks for
// /api/sensors/{fan,power,thermal}. See bb_sensor_http_wire_priv.h for the
// resource-shape contract. Compiles on both host and ESP-IDF.
//
// Thin binding layer only -- every gather/apply hook below calls straight
// into bb_sensor (components/bb_sensor)'s domain snapshot getters / fan
// write path and copies fields into the wire struct (renaming
// bb_sensor_fan_snapshot_t's aux_target_c to this layer's own vr_target_c
// wire name). No HAL calls (bb_fan_*/bb_power_*/bb_temp_*) happen here.
#include "bb_sensor_http_wire_priv.h"

#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_sensor.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fan
// ---------------------------------------------------------------------------

#ifdef CONFIG_BB_FAN_AUTOFAN
static const bb_serialize_field_t s_sensors_fan_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensor_http_fan_wire_t, present) },
    { .key = "autofan", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensor_http_fan_wire_t, autofan) },
    { .key = "die_target_c", .type = BB_TYPE_F64,
      .offset = offsetof(bb_sensor_http_fan_wire_t, die_target_c) },
    { .key = "vr_target_c", .type = BB_TYPE_F64,
      .offset = offsetof(bb_sensor_http_fan_wire_t, vr_target_c) },
    { .key = "manual_pct", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensor_http_fan_wire_t, manual_pct) },
    { .key = "min_pct", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensor_http_fan_wire_t, min_pct) },
};
#else
static const bb_serialize_field_t s_sensors_fan_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensor_http_fan_wire_t, present) },
    { .key = "duty_pct", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensor_http_fan_wire_t, duty_pct) },
};
#endif

const bb_serialize_desc_t bb_sensor_http_fan_wire_desc = {
    .type_name = "sensors_fan",
    .fields    = s_sensors_fan_fields,
    .n_fields  = sizeof(s_sensors_fan_fields) / sizeof(s_sensors_fan_fields[0]),
    .snap_size = sizeof(bb_sensor_http_fan_wire_t),
};

bb_err_t bb_sensor_http_fan_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    bb_sensor_http_fan_wire_t *w = (bb_sensor_http_fan_wire_t *)dst;
    memset(w, 0, sizeof(*w));

    bb_sensor_fan_snapshot_t snap;
    bb_sensor_fan_snapshot(&snap);

#ifdef CONFIG_BB_FAN_AUTOFAN
    w->present      = snap.present;
    w->autofan      = snap.autofan;
    w->die_target_c = (double)snap.die_target_c;
    w->vr_target_c  = (double)snap.aux_target_c;  // wire rename: aux_target_c -> vr_target_c
    w->manual_pct   = (int64_t)snap.manual_pct;
    w->min_pct      = (int64_t)snap.min_pct;
#else
    w->present  = snap.present;
    w->duty_pct = (int64_t)snap.duty_pct;
#endif
    return BB_OK;
}

bb_err_t bb_sensor_http_fan_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    const bb_sensor_http_fan_wire_t *w = (const bb_sensor_http_fan_wire_t *)snap;

    bb_sensor_fan_snapshot_t cfg;
#ifdef CONFIG_BB_FAN_AUTOFAN
    cfg.present      = w->present;
    cfg.autofan      = w->autofan;
    cfg.die_target_c = (float)w->die_target_c;
    cfg.aux_target_c = (float)w->vr_target_c;  // wire rename: vr_target_c -> aux_target_c
    cfg.manual_pct   = (int)w->manual_pct;
    cfg.min_pct      = (int)w->min_pct;
#else
    cfg.present  = w->present;
    cfg.duty_pct = (int)w->duty_pct;
#endif
    return bb_sensor_fan_apply(&cfg);
}

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

// Per-field -1-sentinel gates -- each field is present when its raw
// reading is >= 0, absent (null) when -1. Independent of the
// top-level "present" flag: even with a primary handle, an individual
// channel read can still fail and report its own -1.
static bool power_vout_mv_present(const void *snap)
{ return ((const bb_sensor_http_power_wire_t *)snap)->vout_mv >= 0; }
static bool power_iout_ma_present(const void *snap)
{ return ((const bb_sensor_http_power_wire_t *)snap)->iout_ma >= 0; }
static bool power_pout_mw_present(const void *snap)
{ return ((const bb_sensor_http_power_wire_t *)snap)->pout_mw >= 0; }
static bool power_vin_mv_present(const void *snap)
{ return ((const bb_sensor_http_power_wire_t *)snap)->vin_mv >= 0; }
static bool power_temp_c_present(const void *snap)
{ return ((const bb_sensor_http_power_wire_t *)snap)->temp_c >= 0; }

static const bb_serialize_field_t s_sensors_power_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensor_http_power_wire_t, present) },
    { .key = "vout_mv", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensor_http_power_wire_t, vout_mv), .present = power_vout_mv_present },
    { .key = "iout_ma", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensor_http_power_wire_t, iout_ma), .present = power_iout_ma_present },
    { .key = "pout_mw", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensor_http_power_wire_t, pout_mw), .present = power_pout_mw_present },
    { .key = "vin_mv", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensor_http_power_wire_t, vin_mv), .present = power_vin_mv_present },
    { .key = "temp_c", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensor_http_power_wire_t, temp_c), .present = power_temp_c_present },
};

const bb_serialize_desc_t bb_sensor_http_power_wire_desc = {
    .type_name = "sensors_power",
    .fields    = s_sensors_power_fields,
    .n_fields  = sizeof(s_sensors_power_fields) / sizeof(s_sensors_power_fields[0]),
    .snap_size = sizeof(bb_sensor_http_power_wire_t),
};

bb_err_t bb_sensor_http_power_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    bb_sensor_http_power_wire_t *w = (bb_sensor_http_power_wire_t *)dst;

    bb_sensor_power_snapshot_t snap;
    bb_sensor_power_snapshot(&snap);

    w->present = snap.present;
    w->vout_mv = (int64_t)snap.vout_mv;
    w->iout_ma = (int64_t)snap.iout_ma;
    w->pout_mw = (int64_t)snap.pout_mw;
    w->vin_mv  = (int64_t)snap.vin_mv;
    w->temp_c  = (int64_t)snap.temp_c;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Thermal
// ---------------------------------------------------------------------------

static bool thermal_source_c_present(const void *snap)
{ return ((const bb_sensor_http_thermal_source_wire_t *)snap)->present; }

static const bb_serialize_field_t s_sensors_thermal_source_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensor_http_thermal_source_wire_t, present) },
    { .key = "c", .type = BB_TYPE_F64,
      .offset = offsetof(bb_sensor_http_thermal_source_wire_t, c),
      .present = thermal_source_c_present },
};

#define SENSORS_THERMAL_SOURCE_FIELDS s_sensors_thermal_source_fields
#define SENSORS_THERMAL_SOURCE_N_FIELDS \
    (sizeof(s_sensors_thermal_source_fields) / sizeof(s_sensors_thermal_source_fields[0]))

static const bb_serialize_field_t s_sensors_thermal_fields[] = {
    { .key = "soc", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_sensor_http_thermal_wire_t, soc),
      .children = SENSORS_THERMAL_SOURCE_FIELDS, .n_children = SENSORS_THERMAL_SOURCE_N_FIELDS },
    { .key = "vr", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_sensor_http_thermal_wire_t, vr),
      .children = SENSORS_THERMAL_SOURCE_FIELDS, .n_children = SENSORS_THERMAL_SOURCE_N_FIELDS },
    { .key = "asic", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_sensor_http_thermal_wire_t, asic),
      .children = SENSORS_THERMAL_SOURCE_FIELDS, .n_children = SENSORS_THERMAL_SOURCE_N_FIELDS },
    { .key = "board", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_sensor_http_thermal_wire_t, board),
      .children = SENSORS_THERMAL_SOURCE_FIELDS, .n_children = SENSORS_THERMAL_SOURCE_N_FIELDS },
};

const bb_serialize_desc_t bb_sensor_http_thermal_wire_desc = {
    .type_name = "sensors_thermal",
    .fields    = s_sensors_thermal_fields,
    .n_fields  = sizeof(s_sensors_thermal_fields) / sizeof(s_sensors_thermal_fields[0]),
    .snap_size = sizeof(bb_sensor_http_thermal_wire_t),
};

bb_err_t bb_sensor_http_thermal_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    bb_sensor_http_thermal_wire_t *w = (bb_sensor_http_thermal_wire_t *)dst;

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    w->soc.present   = v.soc_present;
    w->soc.c         = (double)v.soc_c;
    w->vr.present    = v.vr_present;
    w->vr.c          = (double)v.vr_c;
    w->asic.present  = v.asic_present;
    w->asic.c        = (double)v.asic_c;
    w->board.present = v.board_present;
    w->board.c       = (double)v.board_c;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// JSON Schema (B1-1180 PR-2) -- hand-authored, on-device (not host-gated;
// see bb_sensor_http_wire_priv.h's doc comment). Byte-fidelity is proven by
// test/test_host/test_bb_sensor_http_wire_meta_golden.c against TWO
// sources, per CONFIG_BB_FAN_AUTOFAN fork (review fix, HIGH 2): the real
// production `bb_sensor_http_fan_wire_desc` (below) is golden-tested for
// whichever variant this build's Kconfig actually selects as ACTIVE; a
// self-contained shape-desc TWIN (`bb_sensor_http_fan_{autofan,manual}_shape_desc`,
// further down this file) covers the INACTIVE variant -- the one that
// cannot compile as production in this same build, so it has no real desc
// to test against. This is a dark-branch accommodation (host CI never
// exercises the non-default Kconfig branch, B1-1093), not a replacement for
// production coverage: the twin is deliberately never substituted in place
// of the real descriptor for whichever variant IS active.
//
// A #define per literal (not just the extern `_schema` variable below) so
// the describe-only routes further down this file can use the SAME literal
// text as a genuine compile-time constant expression -- `.schema =
// bb_sensor_http_fan_schema` (the VARIABLE's runtime value) is NOT a valid
// static/file-scope initializer in C; `.schema =
// BB_SENSOR_HTTP_FAN_SCHEMA_LITERAL` (the macro-expanded string literal) is
// (mirrors bb_diag_storage_nvs.c's exact precedent, B1-1180 PR-1).
// ---------------------------------------------------------------------------

// Both shapes' literal text is ALWAYS defined here (unconditionally, not
// #ifdef CONFIG_BB_FAN_AUTOFAN-forked) -- B1-1180 PR-2 review fix (HIGH 2):
// this lets the host-only meta/golden layer below byte-verify BOTH the
// autofan (6-field) and manual-duty (2-field, the Kconfig DEFAULT --
// CONFIG_BB_FAN_AUTOFAN defaults to n) shapes on every host run, regardless
// of which one this build's Kconfig/native-env override actually selects
// for production. Only ONE of the two ever gets referenced by production
// code (the BB_SENSOR_HTTP_FAN_SCHEMA_LITERAL/_REQUEST selection just below),
// so only that one's text is ever compiled into a real device build --
// zero device-byte cost for the unreferenced shape's macro text.
#define BB_SENSOR_HTTP_FAN_AUTOFAN_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"present\":{\"type\":\"boolean\"}," \
    "\"autofan\":{\"type\":\"boolean\"}," \
    "\"die_target_c\":{\"type\":\"number\"}," \
    "\"vr_target_c\":{\"type\":\"number\"}," \
    "\"manual_pct\":{\"type\":\"integer\"}," \
    "\"min_pct\":{\"type\":\"integer\"}}," \
    "\"required\":[\"present\",\"autofan\",\"die_target_c\",\"vr_target_c\"," \
    "\"manual_pct\",\"min_pct\"]," \
    "\"additionalProperties\":false}"
#define BB_SENSOR_HTTP_FAN_AUTOFAN_REQUEST_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"present\":{\"type\":\"boolean\"}," \
    "\"autofan\":{\"type\":\"boolean\"}," \
    "\"die_target_c\":{\"type\":\"number\"}," \
    "\"vr_target_c\":{\"type\":\"number\"}," \
    "\"manual_pct\":{\"type\":\"integer\"}," \
    "\"min_pct\":{\"type\":\"integer\"}}," \
    "\"required\":[]," \
    "\"additionalProperties\":false}"
#define BB_SENSOR_HTTP_FAN_MANUAL_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"present\":{\"type\":\"boolean\"}," \
    "\"duty_pct\":{\"type\":\"integer\"}}," \
    "\"required\":[\"present\",\"duty_pct\"]," \
    "\"additionalProperties\":false}"
#define BB_SENSOR_HTTP_FAN_MANUAL_REQUEST_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"present\":{\"type\":\"boolean\"}," \
    "\"duty_pct\":{\"type\":\"integer\"}}," \
    "\"required\":[]," \
    "\"additionalProperties\":false}"

// Production selection -- picks the ONE shape this build's Kconfig actually
// compiles into bb_sensor_http_fan_wire_t/_wire_desc (bb_sensor_http_wire_priv.h).
#ifdef CONFIG_BB_FAN_AUTOFAN
#define BB_SENSOR_HTTP_FAN_SCHEMA_LITERAL         BB_SENSOR_HTTP_FAN_AUTOFAN_SCHEMA_LITERAL
#define BB_SENSOR_HTTP_FAN_REQUEST_SCHEMA_LITERAL BB_SENSOR_HTTP_FAN_AUTOFAN_REQUEST_SCHEMA_LITERAL
#else
#define BB_SENSOR_HTTP_FAN_SCHEMA_LITERAL         BB_SENSOR_HTTP_FAN_MANUAL_SCHEMA_LITERAL
#define BB_SENSOR_HTTP_FAN_REQUEST_SCHEMA_LITERAL BB_SENSOR_HTTP_FAN_MANUAL_REQUEST_SCHEMA_LITERAL
#endif

#define BB_SENSOR_HTTP_POWER_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"present\":{\"type\":\"boolean\"}," \
    "\"vout_mv\":{\"type\":\"integer\"}," \
    "\"iout_ma\":{\"type\":\"integer\"}," \
    "\"pout_mw\":{\"type\":\"integer\"}," \
    "\"vin_mv\":{\"type\":\"integer\"}," \
    "\"temp_c\":{\"type\":\"integer\"}}," \
    "\"required\":[\"present\"]," \
    "\"additionalProperties\":false}"

// Shared shape for each of the four nested thermal sources (soc/vr/asic/
// board) -- "present" always emitted, "c" gated by thermal_source_c_present().
#define BB_SENSOR_HTTP_THERMAL_SOURCE_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"present\":{\"type\":\"boolean\"}," \
    "\"c\":{\"type\":\"number\"}}," \
    "\"required\":[\"present\"]," \
    "\"additionalProperties\":false}"

#define BB_SENSOR_HTTP_THERMAL_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"soc\":" BB_SENSOR_HTTP_THERMAL_SOURCE_SCHEMA_LITERAL "," \
    "\"vr\":" BB_SENSOR_HTTP_THERMAL_SOURCE_SCHEMA_LITERAL "," \
    "\"asic\":" BB_SENSOR_HTTP_THERMAL_SOURCE_SCHEMA_LITERAL "," \
    "\"board\":" BB_SENSOR_HTTP_THERMAL_SOURCE_SCHEMA_LITERAL "}," \
    "\"required\":[\"soc\",\"vr\",\"asic\",\"board\"]," \
    "\"additionalProperties\":false}"

const char *const bb_sensor_http_fan_schema         = BB_SENSOR_HTTP_FAN_SCHEMA_LITERAL;
const char *const bb_sensor_http_fan_request_schema = BB_SENSOR_HTTP_FAN_REQUEST_SCHEMA_LITERAL;
const char *const bb_sensor_http_power_schema       = BB_SENSOR_HTTP_POWER_SCHEMA_LITERAL;
const char *const bb_sensor_http_thermal_schema     = BB_SENSOR_HTTP_THERMAL_SCHEMA_LITERAL;

#if defined(BB_SERIALIZE_META_SHIP)

// ---------------------------------------------------------------------------
// Fan meta/golden layer (B1-1180 PR-2 review fix, HIGH 2 restructure):
//
//   - PRODUCTION (always tested): `bb_sensor_http_fan_meta` /
//     `bb_sensor_http_fan_request_meta` below are #ifdef CONFIG_BB_FAN_AUTOFAN
//     ALIASES -- each just wraps whichever variant's row table (the SAME
//     `s_sensors_fan_{autofan,manual}_*_rows` arrays the twin descs below
//     also use, no content duplication) that this build's Kconfig actually
//     compiles into the real `bb_sensor_http_fan_wire_desc`. Paired with
//     that real descriptor, these are what
//     test_bb_sensor_http_wire_meta_golden.c golden-tests for the ACTIVE
//     variant -- genuine production coverage, not a twin.
//
//   - TWIN (dark-branch only, B1-1093): `bb_sensor_http_fan_autofan_shape_desc`
//     / `bb_sensor_http_fan_manual_shape_desc` are self-contained
//     bb_serialize_desc_t tables, each independent of, and never the same
//     object as, the real production descriptor -- their `.offset` fields
//     are unused dummies (0): the schema composer/validator
//     (bb_serialize_meta_openapi_schema/bb_serialize_meta_validate) only
//     ever reads .key/.type/.children/.max_len/.elem_type, never .offset or
//     a desc's snap_size, so a zero-offset shape table is exactly as valid
//     an input as the real wire struct's offsetof()-derived one. This whole
//     block is host-only (BB_SERIALIZE_META_HOST never defined on-device),
//     so there is zero device-byte cost to compiling BOTH shape descs here
//     unconditionally -- only ONE of bb_sensor_http_fan_wire_t/_wire_desc
//     (bb_sensor_http_wire_priv.h) ever compiles per build (Kconfig-
//     selected). test_bb_sensor_http_wire_meta_golden.c only exercises
//     whichever twin is the CURRENTLY-INACTIVE variant (the opposite
//     #ifdef from the one selecting production above) -- the active
//     variant already gets real production coverage, so testing its twin
//     too would be redundant, and testing it INSTEAD of production (the
//     prior, over-corrected shape of this fix) would leave production's
//     own meta table unverified.
// ---------------------------------------------------------------------------

const char *const bb_sensor_http_fan_autofan_schema = BB_SENSOR_HTTP_FAN_AUTOFAN_SCHEMA_LITERAL;
const char *const bb_sensor_http_fan_manual_schema  = BB_SENSOR_HTTP_FAN_MANUAL_SCHEMA_LITERAL;

static const bb_serialize_field_t s_sensors_fan_autofan_shape_fields[] = {
    { .key = "present",      .type = BB_TYPE_BOOL },
    { .key = "autofan",      .type = BB_TYPE_BOOL },
    { .key = "die_target_c", .type = BB_TYPE_F64 },
    { .key = "vr_target_c",  .type = BB_TYPE_F64 },
    { .key = "manual_pct",   .type = BB_TYPE_I64 },
    { .key = "min_pct",      .type = BB_TYPE_I64 },
};

const bb_serialize_desc_t bb_sensor_http_fan_autofan_shape_desc = {
    .type_name = "sensors_fan",
    .fields    = s_sensors_fan_autofan_shape_fields,
    .n_fields  = sizeof(s_sensors_fan_autofan_shape_fields) / sizeof(s_sensors_fan_autofan_shape_fields[0]),
};

static const bb_serialize_field_meta_t s_sensors_fan_autofan_meta_rows[] = {
    { .key = "present",      .required = true },
    { .key = "autofan",      .required = true },
    { .key = "die_target_c", .required = true },
    { .key = "vr_target_c",  .required = true },
    { .key = "manual_pct",   .required = true },
    { .key = "min_pct",      .required = true },
};

const bb_serialize_desc_meta_t bb_sensor_http_fan_autofan_meta = {
    .type_name = "sensors_fan",
    .rows      = s_sensors_fan_autofan_meta_rows,
    .n_rows    = sizeof(s_sensors_fan_autofan_meta_rows) / sizeof(s_sensors_fan_autofan_meta_rows[0]),
};

// PATCH request rows -- SAME shape as the GET response rows above (fan is a
// round-trip CONFIG resource) but nothing is required: a PATCH is a partial
// update (BB_DATA_APPLY_PATCH -- an omitted field keeps its gathered/seeded
// value, see bb_sensor_http_dispatch.c's sensors_apply()). Only ever
// consumed by the production request-meta alias below (no standalone twin
// test for either shape's PATCH side -- see this section's banner).
static const bb_serialize_field_meta_t s_sensors_fan_autofan_request_meta_rows[] = {
    { .key = "present" },
    { .key = "autofan" },
    { .key = "die_target_c" },
    { .key = "vr_target_c" },
    { .key = "manual_pct" },
    { .key = "min_pct" },
};

static const bb_serialize_field_t s_sensors_fan_manual_shape_fields[] = {
    { .key = "present",  .type = BB_TYPE_BOOL },
    { .key = "duty_pct", .type = BB_TYPE_I64 },
};

const bb_serialize_desc_t bb_sensor_http_fan_manual_shape_desc = {
    .type_name = "sensors_fan",
    .fields    = s_sensors_fan_manual_shape_fields,
    .n_fields  = sizeof(s_sensors_fan_manual_shape_fields) / sizeof(s_sensors_fan_manual_shape_fields[0]),
};

static const bb_serialize_field_meta_t s_sensors_fan_manual_meta_rows[] = {
    { .key = "present",  .required = true },
    { .key = "duty_pct", .required = true },
};

const bb_serialize_desc_meta_t bb_sensor_http_fan_manual_meta = {
    .type_name = "sensors_fan",
    .rows      = s_sensors_fan_manual_meta_rows,
    .n_rows    = sizeof(s_sensors_fan_manual_meta_rows) / sizeof(s_sensors_fan_manual_meta_rows[0]),
};

static const bb_serialize_field_meta_t s_sensors_fan_manual_request_meta_rows[] = {
    { .key = "present" },
    { .key = "duty_pct" },
};

// Production aliases (B1-1180 PR-2 review fix, HIGH 2) -- wrap whichever
// variant's row tables above this build's Kconfig actually compiles into
// the real bb_sensor_http_fan_wire_desc, so test_bb_sensor_http_wire_meta_golden.c
// can golden-test PRODUCTION (not a twin) for the active variant. Same
// #ifdef CONFIG_BB_FAN_AUTOFAN selector as bb_sensor_http_fan_wire_desc
// itself (bb_sensor_http_wire_priv.h) and the schema-literal selection
// above -- all three always agree on which shape is "active".
#ifdef CONFIG_BB_FAN_AUTOFAN
const bb_serialize_desc_meta_t bb_sensor_http_fan_meta = {
    .type_name = "sensors_fan",
    .rows      = s_sensors_fan_autofan_meta_rows,
    .n_rows    = sizeof(s_sensors_fan_autofan_meta_rows) / sizeof(s_sensors_fan_autofan_meta_rows[0]),
};
const bb_serialize_desc_meta_t bb_sensor_http_fan_request_meta = {
    .type_name = "sensors_fan",
    .rows      = s_sensors_fan_autofan_request_meta_rows,
    .n_rows    = sizeof(s_sensors_fan_autofan_request_meta_rows) / sizeof(s_sensors_fan_autofan_request_meta_rows[0]),
};
#else
const bb_serialize_desc_meta_t bb_sensor_http_fan_meta = {
    .type_name = "sensors_fan",
    .rows      = s_sensors_fan_manual_meta_rows,
    .n_rows    = sizeof(s_sensors_fan_manual_meta_rows) / sizeof(s_sensors_fan_manual_meta_rows[0]),
};
const bb_serialize_desc_meta_t bb_sensor_http_fan_request_meta = {
    .type_name = "sensors_fan",
    .rows      = s_sensors_fan_manual_request_meta_rows,
    .n_rows    = sizeof(s_sensors_fan_manual_request_meta_rows) / sizeof(s_sensors_fan_manual_request_meta_rows[0]),
};
#endif

static const bb_serialize_field_meta_t s_sensors_power_meta_rows[] = {
    { .key = "present",  .required = true },
    { .key = "vout_mv" },
    { .key = "iout_ma" },
    { .key = "pout_mw" },
    { .key = "vin_mv" },
    { .key = "temp_c" },
};

const bb_serialize_desc_meta_t bb_sensor_http_power_meta = {
    .type_name = "sensors_power",
    .rows      = s_sensors_power_meta_rows,
    .n_rows    = sizeof(s_sensors_power_meta_rows) / sizeof(s_sensors_power_meta_rows[0]),
};

static const bb_serialize_field_meta_t s_sensors_thermal_source_meta_rows[] = {
    { .key = "present", .required = true },
    { .key = "c" },
};

#define SENSORS_THERMAL_SOURCE_META_ROWS s_sensors_thermal_source_meta_rows
#define SENSORS_THERMAL_SOURCE_META_N_ROWS \
    (sizeof(s_sensors_thermal_source_meta_rows) / sizeof(s_sensors_thermal_source_meta_rows[0]))

static const bb_serialize_field_meta_t s_sensors_thermal_meta_rows[] = {
    { .key = "soc", .required = true,
      .children = SENSORS_THERMAL_SOURCE_META_ROWS, .n_children = SENSORS_THERMAL_SOURCE_META_N_ROWS },
    { .key = "vr", .required = true,
      .children = SENSORS_THERMAL_SOURCE_META_ROWS, .n_children = SENSORS_THERMAL_SOURCE_META_N_ROWS },
    { .key = "asic", .required = true,
      .children = SENSORS_THERMAL_SOURCE_META_ROWS, .n_children = SENSORS_THERMAL_SOURCE_META_N_ROWS },
    { .key = "board", .required = true,
      .children = SENSORS_THERMAL_SOURCE_META_ROWS, .n_children = SENSORS_THERMAL_SOURCE_META_N_ROWS },
};

const bb_serialize_desc_meta_t bb_sensor_http_thermal_meta = {
    .type_name = "sensors_thermal",
    .rows      = s_sensors_thermal_meta_rows,
    .n_rows    = sizeof(s_sensors_thermal_meta_rows) / sizeof(s_sensors_thermal_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// Describe-only routes (B1-1180 PR-2) -- PRODUCER-OWNED `static const`
// bb_route_t entries (handler=NULL), .rodata/flash, never DRAM. Registered
// via bb_http_register_route_descriptor_only() from
// bb_sensor_http_describe_routes(), called once by bb_sensor_http_init()
// (platform/espidf/bb_sensor_http/bb_sensor_http.c) after
// bb_http_section_init() -- never touches the live GET+PATCH
// /api/sensors/* wildcard dispatch bb_http_section_init() itself registers.
//
// power/thermal are GET-only here: both bindings are gather-only (no apply
// hook, see bb_sensor_http_bind_and_register()), so PATCH already 405s at
// the dispatch layer -- describing a PATCH capability that doesn't exist
// would misrepresent the API, so neither gets one.
//
// CONFIG_BB_OPENAPI_RUNTIME_META (B1-1059 PR-3 batch 3) -- gated DIRECTLY on
// this Kconfig symbol, never on BB_SERIALIZE_META_SHIP: that macro also
// covers BB_SERIALIZE_META_HOST (unconditionally set by the plain `native`
// host env, see platformio.ini), which must NOT flip these routes onto the
// runtime-compose path -- "meta tables compiled in for golden-testing"
// (SHIP) and "these routes source their schema at runtime" (RUNTIME_META)
// are deliberately distinct gates. Config OFF (default) is a zero-diff
// no-op: every `#else` arm below is byte-identical to the pre-batch table.
//
// Fan carries TWO independently-composed buffers -- the GET response schema
// and the PATCH request schema -- since it's the only round-trip CONFIG
// resource here; power/thermal are GET-only telemetry (a single response
// schema each).
// ---------------------------------------------------------------------------

#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)

// Sized to the golden-tested hand literal
// (test_bb_sensor_http_wire_meta_golden.c proves the engine's output is
// byte-identical to BB_SENSOR_HTTP_FAN_SCHEMA_LITERAL) -- not an arbitrary
// round cap; any future desync trips bb_serialize_meta_openapi_schema()'s
// own bounded-buffer BB_ERR_NO_SPACE contract instead of silently
// truncating.
static char s_sensors_fan_schema_buf[sizeof(BB_SENSOR_HTTP_FAN_SCHEMA_LITERAL)];
static char s_sensors_fan_request_schema_buf[sizeof(BB_SENSOR_HTTP_FAN_REQUEST_SCHEMA_LITERAL)];
static char s_sensors_power_schema_buf[sizeof(BB_SENSOR_HTTP_POWER_SCHEMA_LITERAL)];
static char s_sensors_thermal_schema_buf[sizeof(BB_SENSOR_HTTP_THERMAL_SCHEMA_LITERAL)];

// Mutable (`.data`, not `.rodata`) with this config on -- each `.schema`/
// `.request_schema` starts NULL and is patched in once by
// bb_sensor_http_describe_routes() (or the test accessors below) before any
// route is ever registered/served.
static bb_route_response_t s_sensors_fan_get_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL /* patched at init */ },
    { .status = 0 },
};

static const bb_route_t s_sensors_fan_get_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/sensors/fan",
    .tag       = "sensors",
    .summary   = "fan configuration (autofan targets, or manual duty)",
    .responses = s_sensors_fan_get_responses,
    .handler   = NULL,
};

static const bb_route_response_t s_sensors_fan_patch_responses[] = {
    { .status = 204, .content_type = NULL, .schema = NULL, .description = "applied" },
    { .status = 400, .content_type = "application/json", .schema = NULL, .description = "invalid request body" },
    { .status = 503, .content_type = "application/json", .schema = NULL, .description = "no primary fan present" },
    { .status = 0 },
};

// `.request_schema` points DIRECTLY at the compose buffer above (a
// link-time-constant array address, valid in a static initializer) rather
// than starting NULL-then-patched like the `.schema` sites above -- the
// mutating-route-needs-body-schema lint (B1-413) flags a literal
// `.request_schema = NULL` on any POST/PATCH/PUT route with a JSON body
// unconditionally (it has no "patched at init" exemption, unlike this
// engine's own buf[0] idempotency-sentinel idiom), but trusts an identifier
// reference (can't inspect a runtime-composed schema's content statically)
// -- exactly this case. The route struct itself stays `static const`: only
// the BYTES the pointer targets change at runtime (via
// ensure_fan_request_schema_patched() below), never the pointer value.
// Before the first compose, the buffer is all-zero (`buf[0] == '\0'`, an
// empty string, never a NULL pointer) -- ensure_fan_request_schema_patched()
// and its test accessor treat that empty-string state as "not yet composed",
// matching bb_serialize_meta_ensure_composed()'s own sentinel contract.
static const bb_route_t s_sensors_fan_patch_describe_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/sensors/fan",
    .tag                  = "sensors",
    .summary              = "update fan configuration (partial update)",
    .request_content_type = "application/json",
    .request_schema       = s_sensors_fan_request_schema_buf,
    .responses            = s_sensors_fan_patch_responses,
    .handler              = NULL,
};

static bb_route_response_t s_sensors_power_get_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL /* patched at init */ },
    { .status = 0 },
};

static const bb_route_t s_sensors_power_get_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/sensors/power",
    .tag       = "sensors",
    .summary   = "power telemetry (read-only)",
    .responses = s_sensors_power_get_responses,
    .handler   = NULL,
};

static bb_route_response_t s_sensors_thermal_get_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL /* patched at init */ },
    { .status = 0 },
};

static const bb_route_t s_sensors_thermal_get_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/sensors/thermal",
    .tag       = "sensors",
    .summary   = "thermal telemetry (read-only)",
    .responses = s_sensors_thermal_get_responses,
    .handler   = NULL,
};

#else

static const bb_route_response_t s_sensors_fan_get_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_SENSOR_HTTP_FAN_SCHEMA_LITERAL },
    { .status = 0 },
};

static const bb_route_t s_sensors_fan_get_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/sensors/fan",
    .tag       = "sensors",
    .summary   = "fan configuration (autofan targets, or manual duty)",
    .responses = s_sensors_fan_get_responses,
    .handler   = NULL,
};

static const bb_route_response_t s_sensors_fan_patch_responses[] = {
    { .status = 204, .content_type = NULL, .schema = NULL, .description = "applied" },
    { .status = 400, .content_type = "application/json", .schema = NULL, .description = "invalid request body" },
    { .status = 503, .content_type = "application/json", .schema = NULL, .description = "no primary fan present" },
    { .status = 0 },
};

static const bb_route_t s_sensors_fan_patch_describe_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/sensors/fan",
    .tag                  = "sensors",
    .summary              = "update fan configuration (partial update)",
    .request_content_type = "application/json",
    .request_schema       = BB_SENSOR_HTTP_FAN_REQUEST_SCHEMA_LITERAL,
    .responses            = s_sensors_fan_patch_responses,
    .handler              = NULL,
};

static const bb_route_response_t s_sensors_power_get_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_SENSOR_HTTP_POWER_SCHEMA_LITERAL },
    { .status = 0 },
};

static const bb_route_t s_sensors_power_get_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/sensors/power",
    .tag       = "sensors",
    .summary   = "power telemetry (read-only)",
    .responses = s_sensors_power_get_responses,
    .handler   = NULL,
};

static const bb_route_response_t s_sensors_thermal_get_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_SENSOR_HTTP_THERMAL_SCHEMA_LITERAL },
    { .status = 0 },
};

static const bb_route_t s_sensors_thermal_get_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/sensors/thermal",
    .tag       = "sensors",
    .summary   = "thermal telemetry (read-only)",
    .responses = s_sensors_thermal_get_responses,
    .handler   = NULL,
};

#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

// Shared compose-and-patch steps, one per buffer, called from both
// bb_sensor_http_describe_routes() and the test accessors below: each is
// idempotent (a second call is a pointer-stable no-op, guarded by
// bb_serialize_meta_ensure_composed()'s buf[0] sentinel) and fail-loud
// (propagates the composer's rc without ever patching a partial/NULL
// schema in).
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
static bb_err_t ensure_fan_schema_patched(void)
{
    bb_err_t rc = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                                      &bb_sensor_http_fan_wire_desc, &bb_sensor_http_fan_meta,
                                                      s_sensors_fan_schema_buf,
                                                      sizeof(s_sensors_fan_schema_buf));
    if (rc != BB_OK) return rc;  // fail loud -- never register a NULL/partial schema
    s_sensors_fan_get_responses[0].schema = s_sensors_fan_schema_buf;
    return BB_OK;
}

// No field-patch step here (unlike the three siblings) -- the const
// s_sensors_fan_patch_describe_route's `.request_schema` already points
// directly at s_sensors_fan_request_schema_buf (see that route's own doc
// comment above); composing into the buffer is the only work needed.
static bb_err_t ensure_fan_request_schema_patched(void)
{
    return bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                              &bb_sensor_http_fan_wire_desc, &bb_sensor_http_fan_request_meta,
                                              s_sensors_fan_request_schema_buf,
                                              sizeof(s_sensors_fan_request_schema_buf));
}

static bb_err_t ensure_power_schema_patched(void)
{
    bb_err_t rc = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                                      &bb_sensor_http_power_wire_desc, &bb_sensor_http_power_meta,
                                                      s_sensors_power_schema_buf,
                                                      sizeof(s_sensors_power_schema_buf));
    if (rc != BB_OK) return rc;  // fail loud -- never register a NULL/partial schema
    s_sensors_power_get_responses[0].schema = s_sensors_power_schema_buf;
    return BB_OK;
}

static bb_err_t ensure_thermal_schema_patched(void)
{
    bb_err_t rc = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                                      &bb_sensor_http_thermal_wire_desc, &bb_sensor_http_thermal_meta,
                                                      s_sensors_thermal_schema_buf,
                                                      sizeof(s_sensors_thermal_schema_buf));
    if (rc != BB_OK) return rc;  // fail loud -- never register a NULL/partial schema
    s_sensors_thermal_get_responses[0].schema = s_sensors_thermal_schema_buf;
    return BB_OK;
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

#ifdef ESP_PLATFORM
bb_err_t bb_sensor_http_describe_routes(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    // Compose ALL FOUR schemas first, then register all four -- never
    // interleave (avoids partial-registration on a mid-sequence compose
    // failure).
    bb_err_t compose_rc = ensure_fan_schema_patched();
    if (compose_rc != BB_OK) return compose_rc;
    compose_rc = ensure_fan_request_schema_patched();
    if (compose_rc != BB_OK) return compose_rc;
    compose_rc = ensure_power_schema_patched();
    if (compose_rc != BB_OK) return compose_rc;
    compose_rc = ensure_thermal_schema_patched();
    if (compose_rc != BB_OK) return compose_rc;
#endif

    bb_err_t rc = bb_http_register_route_descriptor_only(&s_sensors_fan_get_describe_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_route_descriptor_only(&s_sensors_fan_patch_describe_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_route_descriptor_only(&s_sensors_power_get_describe_route);
    if (rc != BB_OK) return rc;
    return bb_http_register_route_descriptor_only(&s_sensors_thermal_get_describe_route);
}
#endif /* ESP_PLATFORM */

// ---------------------------------------------------------------------------
// Test-only accessors (BB_SENSOR_HTTP_WIRE_TESTING) -- see
// bb_sensor_http_wire_test.h. Portable (not ESP_PLATFORM-gated): exercises
// the same guarded, idempotent compose-and-patch bb_sensor_http_describe_
// routes() runs, without requiring the ESP-IDF-gated describe_routes()
// itself.
// ---------------------------------------------------------------------------
#ifdef BB_SENSOR_HTTP_WIRE_TESTING
#include "bb_sensor_http_wire_test.h"

bb_err_t bb_sensor_http_wire_assemble_fan_schema_for_test(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return ensure_fan_schema_patched();
#else
    return BB_OK;
#endif
}

bb_err_t bb_sensor_http_wire_assemble_fan_request_schema_for_test(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return ensure_fan_request_schema_patched();
#else
    return BB_OK;
#endif
}

bb_err_t bb_sensor_http_wire_assemble_power_schema_for_test(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return ensure_power_schema_patched();
#else
    return BB_OK;
#endif
}

bb_err_t bb_sensor_http_wire_assemble_thermal_schema_for_test(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return ensure_thermal_schema_patched();
#else
    return BB_OK;
#endif
}

const char *bb_sensor_http_wire_get_fan_describe_schema_for_test(void)
{
    return s_sensors_fan_get_responses[0].schema;
}

const char *bb_sensor_http_wire_get_fan_describe_request_schema_for_test(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    // s_sensors_fan_patch_describe_route.request_schema always points at
    // s_sensors_fan_request_schema_buf (see that route's own doc comment) --
    // an empty buffer (buf[0] == '\0') means "not yet composed", the same
    // sentinel bb_serialize_meta_ensure_composed() itself uses, so report it
    // as NULL rather than an empty string.
    return s_sensors_fan_request_schema_buf[0] != '\0' ? s_sensors_fan_request_schema_buf : NULL;
#else
    return s_sensors_fan_patch_describe_route.request_schema;
#endif
}

const char *bb_sensor_http_wire_get_power_describe_schema_for_test(void)
{
    return s_sensors_power_get_responses[0].schema;
}

const char *bb_sensor_http_wire_get_thermal_describe_schema_for_test(void)
{
    return s_sensors_thermal_get_responses[0].schema;
}
#endif /* BB_SENSOR_HTTP_WIRE_TESTING */
