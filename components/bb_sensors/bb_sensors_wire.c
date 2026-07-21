// bb_sensors_wire -- the bb_data descriptors + gather/apply hooks for
// /api/sensors/{fan,power,thermal}. See bb_sensors_wire_priv.h for the
// resource-shape contract. Compiles on both host and ESP-IDF.
#include "bb_sensors_wire_priv.h"

#include "bb_fan.h"
#include "bb_power.h"
#include "bb_thermal.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fan
// ---------------------------------------------------------------------------

#ifdef CONFIG_BB_FAN_AUTOFAN
static const bb_serialize_field_t s_sensors_fan_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensors_fan_wire_t, present) },
    { .key = "autofan", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensors_fan_wire_t, autofan) },
    { .key = "die_target_c", .type = BB_TYPE_F64,
      .offset = offsetof(bb_sensors_fan_wire_t, die_target_c) },
    { .key = "vr_target_c", .type = BB_TYPE_F64,
      .offset = offsetof(bb_sensors_fan_wire_t, vr_target_c) },
    { .key = "manual_pct", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensors_fan_wire_t, manual_pct) },
    { .key = "min_pct", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensors_fan_wire_t, min_pct) },
};
#else
static const bb_serialize_field_t s_sensors_fan_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensors_fan_wire_t, present) },
    { .key = "duty_pct", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensors_fan_wire_t, duty_pct) },
};
#endif

const bb_serialize_desc_t bb_sensors_fan_wire_desc = {
    .type_name = "sensors_fan",
    .fields    = s_sensors_fan_fields,
    .n_fields  = sizeof(s_sensors_fan_fields) / sizeof(s_sensors_fan_fields[0]),
    .snap_size = sizeof(bb_sensors_fan_wire_t),
};

bb_err_t bb_sensors_fan_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    bb_sensors_fan_wire_t *w = (bb_sensors_fan_wire_t *)dst;
    memset(w, 0, sizeof(*w));

#ifdef CONFIG_BB_FAN_AUTOFAN
    // No primary fan is an ORDINARY hardware state (autofan compiled in,
    // nothing wired yet) -- GET must still succeed (200), reporting absence
    // via `present`, not fail. Also doubles as the PATCH-seed step
    // (BB_DATA_APPLY_PATCH): a real "seed from the live config" read so an
    // absent PATCH field keeps its current value, same as before -- with no
    // primary there is nothing to seed, so the merged fields simply stay at
    // their zeroed defaults (bb_sensors_fan_apply() is what rejects the
    // no-primary PATCH itself, at commit time -- see its own doc).
    bb_fan_handle_t h = bb_fan_primary();
    w->present = (h != NULL);
    if (!h) return BB_OK;

    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(h, &cfg);
    w->autofan       = cfg.enabled;
    w->die_target_c  = (double)cfg.die_target_c;
    w->vr_target_c   = (double)cfg.aux_target_c;
    w->manual_pct    = (int64_t)cfg.manual_pct;
    w->min_pct       = (int64_t)cfg.min_pct;
#else
    // Fixed SENTINEL, deliberately not a real seed. duty_pct is a REQUIRED
    // field on every PATCH in this mode (mirrors the old fan_section_patch
    // contract), so seeding never matters for the merge -- it always gets
    // overwritten. GET reflects this same fixed sentinel; there is no
    // "current duty" concept exposed on this resource in non-autofan mode.
    // `present` still reflects live hardware state (consistent with the
    // autofan branch above), independent of the sentinel.
    w->present  = (bb_fan_primary() != NULL);
    w->duty_pct = -1;
#endif
    return BB_OK;
}

bb_err_t bb_sensors_fan_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    const bb_sensors_fan_wire_t *w = (const bb_sensors_fan_wire_t *)snap;

    bb_fan_handle_t h = bb_fan_primary();
    // No primary fan: BB_ERR_UNSUPPORTED (not BB_ERR_INVALID_STATE) so the
    // shared bb_http_section status mapper's commit-stage override lands
    // this on 503 rather than a bare 500 -- see bb_sensors_dispatch.c's
    // ns.unsupported_status doc. Common to both variants (mirrors the old
    // handler's own no-primary-fan -> 503 behavior).
    if (!h) return BB_ERR_UNSUPPORTED;

#ifdef CONFIG_BB_FAN_AUTOFAN
    // Validates the WHOLE merged struct unconditionally -- correct under
    // BB_DATA_APPLY_PATCH seeding: an absent field carries the gathered
    // (already-valid, currently-live) value, so validating every field is
    // equivalent to the old "validate only fields present in the raw PATCH
    // body" behavior, without needing to inspect which fields the wire body
    // itself supplied.
    if (w->die_target_c <= 0.0)                    return BB_ERR_VALIDATION;
    if (w->vr_target_c <= 0.0)                      return BB_ERR_VALIDATION;
    if (w->manual_pct < 0 || w->manual_pct > 100)    return BB_ERR_VALIDATION;
    if (w->min_pct < 0 || w->min_pct > 100)          return BB_ERR_VALIDATION;

    bb_fan_autofan_cfg_t cfg = {
        .enabled      = w->autofan,
        .die_target_c = (float)w->die_target_c,
        .aux_target_c = (float)w->vr_target_c,
        .min_pct      = (int)w->min_pct,
        .manual_pct   = (int)w->manual_pct,
    };
    return bb_fan_set_autofan(h, &cfg);
#else
    if (w->duty_pct < 0 || w->duty_pct > 100) return BB_ERR_VALIDATION;

    bb_err_t rc = bb_fan_set_duty_pct(h, (int)w->duty_pct);
    // bb_fan_set_duty_pct() has its OWN, independent BB_ERR_UNSUPPORTED --
    // the bound driver's vtable simply has no set_duty_pct (a legitimate,
    // nullable capability gap; see platform/host/bb_fan/bb_fan.c and
    // test/test_host/test_bb_fan.c's drv_minimal coverage). That is a
    // DIFFERENT condition from "no primary fan" above, but shares the same
    // bb_err_t value -- left alone it would collide on the namespace's
    // single unsupported_status override (503, reserved for no-primary; see
    // bb_sensors_dispatch.c and bb_http_section_status.c's single-override
    // constraint), masking a real capability gap as "no fan wired". Retarget
    // to BB_ERR_INVALID_STATE so it falls through the mapper's default 500
    // instead, keeping the two conditions distinguishable on the wire.
    if (rc == BB_ERR_UNSUPPORTED) return BB_ERR_INVALID_STATE;
    return rc;
#endif
}

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

// Per-field -1-sentinel gates -- each field is present when its raw
// reading is >= 0, absent (null) when -1. Independent of the
// top-level "present" flag: even with a primary handle, an individual
// channel read can still fail and report its own -1.
static bool power_vout_mv_present(const void *snap)
{ return ((const bb_sensors_power_wire_t *)snap)->vout_mv >= 0; }
static bool power_iout_ma_present(const void *snap)
{ return ((const bb_sensors_power_wire_t *)snap)->iout_ma >= 0; }
static bool power_pout_mw_present(const void *snap)
{ return ((const bb_sensors_power_wire_t *)snap)->pout_mw >= 0; }
static bool power_vin_mv_present(const void *snap)
{ return ((const bb_sensors_power_wire_t *)snap)->vin_mv >= 0; }
static bool power_temp_c_present(const void *snap)
{ return ((const bb_sensors_power_wire_t *)snap)->temp_c >= 0; }

static const bb_serialize_field_t s_sensors_power_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensors_power_wire_t, present) },
    { .key = "vout_mv", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensors_power_wire_t, vout_mv), .present = power_vout_mv_present },
    { .key = "iout_ma", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensors_power_wire_t, iout_ma), .present = power_iout_ma_present },
    { .key = "pout_mw", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensors_power_wire_t, pout_mw), .present = power_pout_mw_present },
    { .key = "vin_mv", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensors_power_wire_t, vin_mv), .present = power_vin_mv_present },
    { .key = "temp_c", .type = BB_TYPE_I64,
      .offset = offsetof(bb_sensors_power_wire_t, temp_c), .present = power_temp_c_present },
};

const bb_serialize_desc_t bb_sensors_power_wire_desc = {
    .type_name = "sensors_power",
    .fields    = s_sensors_power_fields,
    .n_fields  = sizeof(s_sensors_power_fields) / sizeof(s_sensors_power_fields[0]),
    .snap_size = sizeof(bb_sensors_power_wire_t),
};

bb_err_t bb_sensors_power_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    bb_sensors_power_wire_t *w = (bb_sensors_power_wire_t *)dst;

    bb_power_handle_t h = bb_power_primary();
    bb_power_snapshot_t snap;
    bb_power_snapshot(h, &snap);

    w->present  = (h != NULL);
    w->vout_mv  = (int64_t)snap.vout_mv;
    w->iout_ma  = (int64_t)snap.iout_ma;
    w->pout_mw  = (int64_t)snap.pout_mw;
    w->vin_mv   = (int64_t)snap.vin_mv;
    w->temp_c   = (int64_t)snap.temp_c;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Thermal
// ---------------------------------------------------------------------------

static bool thermal_source_c_present(const void *snap)
{ return ((const bb_sensors_thermal_source_wire_t *)snap)->present; }

static const bb_serialize_field_t s_sensors_thermal_source_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_sensors_thermal_source_wire_t, present) },
    { .key = "c", .type = BB_TYPE_F64,
      .offset = offsetof(bb_sensors_thermal_source_wire_t, c),
      .present = thermal_source_c_present },
};

#define SENSORS_THERMAL_SOURCE_FIELDS s_sensors_thermal_source_fields
#define SENSORS_THERMAL_SOURCE_N_FIELDS \
    (sizeof(s_sensors_thermal_source_fields) / sizeof(s_sensors_thermal_source_fields[0]))

static const bb_serialize_field_t s_sensors_thermal_fields[] = {
    { .key = "soc", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_sensors_thermal_wire_t, soc),
      .children = SENSORS_THERMAL_SOURCE_FIELDS, .n_children = SENSORS_THERMAL_SOURCE_N_FIELDS },
    { .key = "vr", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_sensors_thermal_wire_t, vr),
      .children = SENSORS_THERMAL_SOURCE_FIELDS, .n_children = SENSORS_THERMAL_SOURCE_N_FIELDS },
    { .key = "asic", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_sensors_thermal_wire_t, asic),
      .children = SENSORS_THERMAL_SOURCE_FIELDS, .n_children = SENSORS_THERMAL_SOURCE_N_FIELDS },
    { .key = "board", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_sensors_thermal_wire_t, board),
      .children = SENSORS_THERMAL_SOURCE_FIELDS, .n_children = SENSORS_THERMAL_SOURCE_N_FIELDS },
};

const bb_serialize_desc_t bb_sensors_thermal_wire_desc = {
    .type_name = "sensors_thermal",
    .fields    = s_sensors_thermal_fields,
    .n_fields  = sizeof(s_sensors_thermal_fields) / sizeof(s_sensors_thermal_fields[0]),
    .snap_size = sizeof(bb_sensors_thermal_wire_t),
};

bb_err_t bb_sensors_thermal_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    bb_sensors_thermal_wire_t *w = (bb_sensors_thermal_wire_t *)dst;

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

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
