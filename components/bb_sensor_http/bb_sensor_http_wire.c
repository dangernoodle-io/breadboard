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
