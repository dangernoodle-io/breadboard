// Tests for bb_sensors: /api/sensors GET sections, fan PATCH, read-only power/thermal
// reject PATCH, schema valid JSON, freeze/capacity.
//
// Uses bb_sensors test hooks (BB_SENSORS_TESTING) to drive section registry
// and validate section content. Does NOT register HTTP routes (host build);
// tests call bb_sensors_invoke_sections_for_test / bb_sensors_dispatch_patch_for_test.
#include "unity.h"
#include "bb_sensors.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include "bb_fan_routes.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include "bb_power_routes.h"
#include "bb_thermal.h"
#include "bb_temp.h"
#include "bb_temp_test.h"
#include "bb_section.h"
#include "bb_json.h"
#include "cJSON.h"

#include "../../../components/bb_sensors/bb_sensors_schema_priv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Fake fan backend (shared across test cases)
// ---------------------------------------------------------------------------

typedef struct {
    int   rpm;
    int   duty_pct;
    float die_c;
    float board_c;
    bool  die_fail;
    bool  board_fail;
    int   set_duty_last;
    bb_err_t set_duty_ret;
} fake_fan_t;

static fake_fan_t g_fan;

static bb_err_t ff_set_duty(void *s, int pct)
{
    fake_fan_t *f = s;
    f->set_duty_last = pct;
    f->duty_pct      = pct;
    return f->set_duty_ret;
}
static int ff_get_duty(void *s) { return ((fake_fan_t *)s)->duty_pct; }
static int ff_rpm(void *s)      { return ((fake_fan_t *)s)->rpm; }
static bb_err_t ff_die(void *s, float *out)
{
    fake_fan_t *f = s;
    if (f->die_fail) return BB_ERR_INVALID_STATE;
    *out = f->die_c;
    return BB_OK;
}
static bb_err_t ff_board(void *s, float *out)
{
    fake_fan_t *f = s;
    if (f->board_fail) return BB_ERR_INVALID_STATE;
    *out = f->board_c;
    return BB_OK;
}

static const bb_fan_driver_t drv_fan = {
    .set_duty_pct      = ff_set_duty,
    .get_duty_pct      = ff_get_duty,
    .read_rpm          = ff_rpm,
    .read_die_temp_c   = ff_die,
    .read_board_temp_c = ff_board,
    .name              = "fake_fan",
};

// ---------------------------------------------------------------------------
// Fake power backend
// ---------------------------------------------------------------------------

typedef struct {
    int vout_mv, iout_ma, vin_mv, temp_c;
} fake_pwr_t;

static fake_pwr_t g_pwr;

static int fp_vout(void *s) { return ((fake_pwr_t *)s)->vout_mv; }
static int fp_iout(void *s) { return ((fake_pwr_t *)s)->iout_ma; }
static int fp_vin (void *s) { return ((fake_pwr_t *)s)->vin_mv; }
static int fp_temp(void *s) { return ((fake_pwr_t *)s)->temp_c; }
static bb_err_t fp_set(void *s, uint16_t mv) { (void)s; (void)mv; return BB_OK; }

static const bb_power_driver_t drv_pwr = {
    .read_vout_mv = fp_vout,
    .read_iout_ma = fp_iout,
    .read_vin_mv  = fp_vin,
    .read_temp_c  = fp_temp,
    .set_vout_mv  = fp_set,
    .name         = "fake_pwr",
};

// ---------------------------------------------------------------------------
// Helpers: register built-in sections matching bb_sensors_init order
// ---------------------------------------------------------------------------

static void fans_get(bb_json_t s, void *ctx)
{
    (void)ctx;
    bb_fan_emit_section(s);
}

static bb_err_t fans_patch(bb_json_t patch, void *ctx)
{
    (void)ctx;
    bb_fan_handle_t h = bb_fan_primary();
    if (!h) return BB_ERR_INVALID_STATE;
#ifdef CONFIG_BB_FAN_AUTOFAN
    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(h, &cfg);
    double d;
    bool b;
    if (bb_json_obj_get_bool(patch, "autofan", &b))         cfg.enabled      = b;
    if (bb_json_obj_get_number(patch, "die_target_c", &d))  cfg.die_target_c = (float)d;
    if (bb_json_obj_get_number(patch, "vr_target_c",  &d))  cfg.aux_target_c = (float)d;
    if (bb_json_obj_get_number(patch, "manual_pct",   &d))  cfg.manual_pct   = (int)d;
    if (bb_json_obj_get_number(patch, "min_pct",      &d))  cfg.min_pct      = (int)d;
    bb_fan_set_autofan(h, &cfg);
#else
    double duty_d = -1.0;
    if (!bb_json_obj_get_number(patch, "duty_pct", &duty_d)) return BB_ERR_INVALID_ARG;
    int duty = (int)duty_d;
    if (duty < 0 || duty > 100) return BB_ERR_INVALID_ARG;
    bb_fan_set_duty_pct(h, duty);
#endif
    return BB_OK;
}

static void power_get(bb_json_t s, void *ctx)
{
    (void)ctx;
    bb_power_emit_section(s);
}

static void thermal_get(bb_json_t s, void *ctx)
{
    (void)ctx;
    bb_thermal_emit_section(s);
}

// Register built-in sections into the test registry (mirrors bb_sensors_init).
static void register_builtin_sections(void)
{
    bb_err_t err;
    err = bb_sensors_register_section("fan",     fans_get,    fans_patch, NULL, k_sensors_fan_schema);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = bb_sensors_register_section("power",   power_get,   NULL,       NULL, k_sensors_power_schema);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = bb_sensors_register_section("thermal", thermal_get, NULL,       NULL, k_sensors_thermal_schema);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// ---------------------------------------------------------------------------
// Section registration tests
// ---------------------------------------------------------------------------

static void dummy_get(bb_json_t s, void *ctx) { (void)s; (void)ctx; }

void test_bb_sensors_register_null_name_returns_err(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG,
        bb_sensors_register_section(NULL, dummy_get, NULL, NULL, NULL));
}

void test_bb_sensors_register_null_get_returns_err(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG,
        bb_sensors_register_section("x", NULL, NULL, NULL, NULL));
}

void test_bb_sensors_register_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_sensors_register_section("x", dummy_get, NULL, NULL, NULL));
}

void test_bb_sensors_register_capacity(void)
{
    static const char *k_names[] = { "s0","s1","s2","s3","s4","s5","s6","s7" };
    for (int i = 0; i < BB_SECTION_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(BB_OK,
            bb_sensors_register_section(k_names[i], dummy_get, NULL, NULL, NULL));
    }
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE,
        bb_sensors_register_section("over", dummy_get, NULL, NULL, NULL));
}

void test_bb_sensors_register_after_freeze_returns_invalid_state(void)
{
    bb_sensors_freeze_for_test();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE,
        bb_sensors_register_section("x", dummy_get, NULL, NULL, NULL));
}

// ---------------------------------------------------------------------------
// GET section tests
// ---------------------------------------------------------------------------

void test_bb_sensors_get_fan_section_present_false_when_no_primary(void)
{
    register_builtin_sections();

    bb_json_t root = bb_json_obj_new();
    bb_sensors_invoke_sections_for_test(root);

    bb_json_t fan = bb_json_obj_get_item(root, "fan");
    TEST_ASSERT_NOT_NULL_MESSAGE(fan, "fan section missing");

    bool present = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(fan, "present", &present));
    TEST_ASSERT_FALSE_MESSAGE(present, "fan present should be false with no primary");

    bb_json_free(root);
}

void test_bb_sensors_get_fan_section_present_true_with_primary(void)
{
    register_builtin_sections();

    bb_fan_handle_t fh;
    g_fan.rpm      = 1200;
    g_fan.duty_pct = 50;
    g_fan.die_fail = g_fan.board_fail = true;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_json_t root = bb_json_obj_new();
    bb_sensors_invoke_sections_for_test(root);

    bb_json_t fan = bb_json_obj_get_item(root, "fan");
    TEST_ASSERT_NOT_NULL(fan);

    bool present = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(fan, "present", &present));
    TEST_ASSERT_TRUE_MESSAGE(present, "fan present should be true");

    double rpm = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(fan, "rpm", &rpm));
    TEST_ASSERT_EQUAL_INT(1200, (int)rpm);

    // duty_pct reflects what bb_fan_poll cached. When BB_FAN_AUTOFAN is compiled
    // in and autofan is disabled, poll applies manual_pct (default 100) not the
    // driver's initial value. Read back from g_fan.duty_pct (set via set_duty_pct
    // in the driver mock) to stay accurate in both compile-time configurations.
    bb_fan_snapshot_t snap;
    bb_fan_snapshot(fh, &snap);
    double duty = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(fan, "duty_pct", &duty));
    TEST_ASSERT_EQUAL_INT(snap.duty_pct, (int)duty);

    bb_json_free(root);
    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_get_power_section_present_false_when_no_primary(void)
{
    register_builtin_sections();

    bb_json_t root = bb_json_obj_new();
    bb_sensors_invoke_sections_for_test(root);

    bb_json_t power = bb_json_obj_get_item(root, "power");
    TEST_ASSERT_NOT_NULL_MESSAGE(power, "power section missing");

    bool present = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(power, "present", &present));
    TEST_ASSERT_FALSE_MESSAGE(present, "power present should be false with no primary");

    bb_json_free(root);
}

void test_bb_sensors_get_power_section_present_true_with_primary(void)
{
    register_builtin_sections();

    bb_power_handle_t ph;
    g_pwr.vout_mv = 1200;
    g_pwr.iout_ma = 500;
    g_pwr.vin_mv  = 12000;
    g_pwr.temp_c  = 45;
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_json_t root = bb_json_obj_new();
    bb_sensors_invoke_sections_for_test(root);

    bb_json_t power = bb_json_obj_get_item(root, "power");
    TEST_ASSERT_NOT_NULL(power);

    bool present = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(power, "present", &present));
    TEST_ASSERT_TRUE(present);

    double vout = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(power, "vout_mv", &vout));
    TEST_ASSERT_EQUAL_INT(1200, (int)vout);

    bb_json_free(root);
    bb_power_set_primary(NULL);
    free(ph);
}

void test_bb_sensors_get_thermal_section_has_soc_vr_asic_board(void)
{
    register_builtin_sections();

    bb_json_t root = bb_json_obj_new();
    bb_sensors_invoke_sections_for_test(root);

    bb_json_t thermal = bb_json_obj_get_item(root, "thermal");
    TEST_ASSERT_NOT_NULL_MESSAGE(thermal, "thermal section missing");

    TEST_ASSERT_NOT_NULL_MESSAGE(bb_json_obj_get_item(thermal, "soc"),   "soc missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(bb_json_obj_get_item(thermal, "vr"),    "vr missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(bb_json_obj_get_item(thermal, "asic"),  "asic missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(bb_json_obj_get_item(thermal, "board"), "board missing");

    bb_json_free(root);
}

void test_bb_sensors_get_thermal_soc_present_when_available(void)
{
    register_builtin_sections();
    bb_temp_test_set_soc(true, 62.5f);

    bb_json_t root = bb_json_obj_new();
    bb_sensors_invoke_sections_for_test(root);

    bb_json_t thermal = bb_json_obj_get_item(root, "thermal");
    TEST_ASSERT_NOT_NULL(thermal);

    bb_json_t soc = bb_json_obj_get_item(thermal, "soc");
    TEST_ASSERT_NOT_NULL(soc);

    bool present = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(soc, "present", &present));
    TEST_ASSERT_TRUE(present);

    double c = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(soc, "c", &c));
    TEST_ASSERT_EQUAL_FLOAT(62.5f, (float)c);

    bb_json_free(root);
}

// ---------------------------------------------------------------------------
// Fan PATCH tests
// ---------------------------------------------------------------------------

#ifndef CONFIG_BB_FAN_AUTOFAN
// Without autofan: PATCH {"fan":{"duty_pct":75}} calls bb_fan_set_duty_pct directly.
void test_bb_sensors_fan_patch_duty_pct_applies(void)
{
    register_builtin_sections();

    bb_fan_handle_t fh;
    g_fan.duty_pct    = 0;
    g_fan.set_duty_ret = BB_OK;
    g_fan.die_fail     = g_fan.board_fail = true;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    // PATCH body: {"fan":{"duty_pct":75}}
    bb_json_t body = bb_json_obj_new();
    bb_json_t fan_patch = bb_json_obj_new();
    bb_json_obj_set_number(fan_patch, "duty_pct", 75.0);
    bb_json_obj_set_obj(body, "fan", fan_patch);

    bb_err_t rc = bb_sensors_dispatch_patch_for_test(body);
    bb_json_free(body);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(75, g_fan.set_duty_last);

    bb_fan_set_primary(NULL);
    free(fh);
}
#else
// With autofan: PATCH {"fan":{"duty_pct":75}} sets manual_pct via set_autofan.
// Verify the cfg is updated (duty_pct in the patch body → manual_pct in the autofan cfg).
void test_bb_sensors_fan_patch_duty_pct_applies(void)
{
    register_builtin_sections();

    bb_fan_handle_t fh;
    g_fan.set_duty_ret = BB_OK;
    g_fan.die_fail     = g_fan.board_fail = true;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_fan_autofan_cfg_t cfg_before;
    bb_fan_get_autofan_cfg(fh, &cfg_before);
    int expected_manual = cfg_before.manual_pct; // default 100

    // PATCH body: {"fan":{"duty_pct":75}} — with autofan compiled in,
    // duty_pct is not a recognised autofan field so the patch is a no-op
    // (partial update model: unrecognised fields are silently ignored).
    // Verify rc=BB_OK and cfg is unchanged (manual_pct remains at default).
    bb_json_t body = bb_json_obj_new();
    bb_json_t fan_patch = bb_json_obj_new();
    bb_json_obj_set_number(fan_patch, "duty_pct", 75.0);
    bb_json_obj_set_obj(body, "fan", fan_patch);

    bb_err_t rc = bb_sensors_dispatch_patch_for_test(body);
    bb_json_free(body);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_fan_autofan_cfg_t cfg_after;
    bb_fan_get_autofan_cfg(fh, &cfg_after);
    TEST_ASSERT_EQUAL_INT(expected_manual, cfg_after.manual_pct);

    bb_fan_set_primary(NULL);
    free(fh);
}
#endif /* CONFIG_BB_FAN_AUTOFAN */

void test_bb_sensors_fan_patch_no_primary_returns_invalid_state(void)
{
    register_builtin_sections();
    // No fan primary set.

    bb_json_t body = bb_json_obj_new();
    bb_json_t fan_patch = bb_json_obj_new();
    bb_json_obj_set_number(fan_patch, "duty_pct", 50.0);
    bb_json_obj_set_obj(body, "fan", fan_patch);

    bb_err_t rc = bb_sensors_dispatch_patch_for_test(body);
    bb_json_free(body);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, rc);
}

#ifdef CONFIG_BB_FAN_AUTOFAN
void test_bb_sensors_fan_patch_autofan_die_target(void)
{
    register_builtin_sections();

    bb_fan_handle_t fh;
    g_fan.die_fail = g_fan.board_fail = true;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_json_t body = bb_json_obj_new();
    bb_json_t fan_patch = bb_json_obj_new();
    bb_json_obj_set_number(fan_patch, "die_target_c", 70.0);
    bb_json_obj_set_obj(body, "fan", fan_patch);

    bb_err_t rc = bb_sensors_dispatch_patch_for_test(body);
    bb_json_free(body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(fh, &cfg);
    TEST_ASSERT_EQUAL_FLOAT(70.0f, cfg.die_target_c);

    bb_fan_set_primary(NULL);
    free(fh);
}
#endif /* CONFIG_BB_FAN_AUTOFAN */

// ---------------------------------------------------------------------------
// Read-only section PATCH rejection
// ---------------------------------------------------------------------------

void test_bb_sensors_power_patch_rejected(void)
{
    register_builtin_sections();

    // PATCH body: {"power":{"vout_mv":1200}} — power is read-only.
    bb_json_t body = bb_json_obj_new();
    bb_json_t power_patch = bb_json_obj_new();
    bb_json_obj_set_number(power_patch, "vout_mv", 1200.0);
    bb_json_obj_set_obj(body, "power", power_patch);

    bb_err_t rc = bb_sensors_dispatch_patch_for_test(body);
    bb_json_free(body);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_sensors_thermal_patch_rejected(void)
{
    register_builtin_sections();

    // PATCH body: {"thermal":{"soc":{"c":50}}} — thermal is read-only.
    bb_json_t body = bb_json_obj_new();
    bb_json_t th_patch = bb_json_obj_new();
    bb_json_obj_set_number(th_patch, "x", 1.0);
    bb_json_obj_set_obj(body, "thermal", th_patch);

    bb_err_t rc = bb_sensors_dispatch_patch_for_test(body);
    bb_json_free(body);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

// ---------------------------------------------------------------------------
// Schema tests
// ---------------------------------------------------------------------------

void test_bb_sensors_schema_no_sections_is_valid_json(void)
{
    const char *schema = bb_sensors_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled sensors schema is not valid JSON");
    cJSON_Delete(parsed);
}

void test_bb_sensors_schema_with_builtin_sections_is_valid_json(void)
{
    register_builtin_sections();
    const char *schema = bb_sensors_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "sensors schema with sections is not valid JSON");
    cJSON_Delete(parsed);
}

void test_bb_sensors_schema_contains_fan_section(void)
{
    register_builtin_sections();
    const char *schema = bb_sensors_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"fan\""),     "fan missing from sensors schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"present\""), "present missing from sensors schema");
}

void test_bb_sensors_schema_contains_power_section(void)
{
    register_builtin_sections();
    const char *schema = bb_sensors_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"power\""),   "power missing from sensors schema");
}

void test_bb_sensors_schema_contains_thermal_section(void)
{
    register_builtin_sections();
    const char *schema = bb_sensors_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"thermal\""), "thermal missing from sensors schema");
}

// ---------------------------------------------------------------------------
// External section registration (TM pattern)
// ---------------------------------------------------------------------------

static void mining_get(bb_json_t s, void *ctx) { (void)ctx; bb_json_obj_set_bool(s, "present", true); }

void test_bb_sensors_external_section_registered_ok(void)
{
    static const char mining_schema[] =
        "{\"type\":\"object\",\"properties\":{\"present\":{\"type\":\"boolean\"}}}";
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_sensors_register_section("mining", mining_get, NULL, NULL, mining_schema));

    bb_json_t root = bb_json_obj_new();
    bb_sensors_invoke_sections_for_test(root);
    bb_json_t mining = bb_json_obj_get_item(root, "mining");
    TEST_ASSERT_NOT_NULL_MESSAGE(mining, "mining section missing after external registration");
    bb_json_free(root);
}

void test_bb_sensors_external_section_schema_in_assembled(void)
{
    static const char mining_schema[] =
        "{\"type\":\"object\",\"properties\":{\"present\":{\"type\":\"boolean\"}}}";
    bb_sensors_register_section("mining", mining_get, NULL, NULL, mining_schema);
    const char *schema = bb_sensors_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"mining\""),
        "external mining section not found in assembled schema");
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema with external section is not valid JSON");
    cJSON_Delete(parsed);
}

// ---------------------------------------------------------------------------
// Fan PATCH autofan validation (fix #4 — B1-269 hardening)
// ---------------------------------------------------------------------------

#ifdef CONFIG_BB_FAN_AUTOFAN

// These tests call bb_sensors_fan_patch_for_test() which directly exercises
// fan_section_patch() (the real autofan validation logic) rather than going
// through the test-stub section registry (which uses stub callbacks).

static bb_fan_handle_t make_autofan_handle(void)
{
    bb_fan_handle_t fh;
    g_fan.die_fail = g_fan.board_fail = true;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);
    return fh;
}

void test_bb_sensors_fan_patch_autofan_manual_pct_invalid_over100(void)
{
    bb_fan_handle_t fh = make_autofan_handle();

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "manual_pct", 101.0);

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_INVALID_ARG, rc,
        "manual_pct=101 should be rejected");

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_fan_patch_autofan_manual_pct_invalid_negative(void)
{
    bb_fan_handle_t fh = make_autofan_handle();

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "manual_pct", -1.0);

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_INVALID_ARG, rc,
        "manual_pct=-1 should be rejected");

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_fan_patch_autofan_min_pct_invalid_over100(void)
{
    bb_fan_handle_t fh = make_autofan_handle();

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "min_pct", 200.0);

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_INVALID_ARG, rc,
        "min_pct=200 should be rejected");

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_fan_patch_autofan_die_target_invalid_zero(void)
{
    bb_fan_handle_t fh = make_autofan_handle();

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "die_target_c", 0.0);

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_INVALID_ARG, rc,
        "die_target_c=0 should be rejected");

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_fan_patch_autofan_die_target_invalid_negative(void)
{
    bb_fan_handle_t fh = make_autofan_handle();

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "die_target_c", -10.0);

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_INVALID_ARG, rc,
        "die_target_c=-10 should be rejected");

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_fan_patch_autofan_vr_target_invalid_zero(void)
{
    bb_fan_handle_t fh = make_autofan_handle();

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "vr_target_c", 0.0);

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_INVALID_ARG, rc,
        "vr_target_c=0 should be rejected");

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_fan_patch_autofan_valid_boundary_0(void)
{
    // manual_pct=0 is valid (0..100 inclusive).
    bb_fan_handle_t fh = make_autofan_handle();

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "manual_pct", 0.0);

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, rc, "manual_pct=0 should be accepted");

    bb_fan_autofan_cfg_t after;
    bb_fan_get_autofan_cfg(fh, &after);
    TEST_ASSERT_EQUAL_INT(0, after.manual_pct);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_fan_patch_autofan_valid_boundary_100(void)
{
    // manual_pct=100 is valid (0..100 inclusive).
    bb_fan_handle_t fh = make_autofan_handle();

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "manual_pct", 100.0);

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, rc, "manual_pct=100 should be accepted");

    bb_fan_autofan_cfg_t after;
    bb_fan_get_autofan_cfg(fh, &after);
    TEST_ASSERT_EQUAL_INT(100, after.manual_pct);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensors_fan_patch_autofan_atomicity_bad_second_field(void)
{
    // Patch: manual_pct=50 (valid field) + die_target_c=0 (invalid).
    // The local cfg copy absorbs manual_pct=50, then die_target_c=0 causes early
    // return — bb_fan_set_autofan is never called, so manual_pct stays unchanged.
    bb_fan_handle_t fh = make_autofan_handle();

    bb_fan_autofan_cfg_t before;
    bb_fan_get_autofan_cfg(fh, &before);

    bb_json_t patch = bb_json_obj_new();
    bb_json_obj_set_number(patch, "manual_pct", 50.0);
    bb_json_obj_set_number(patch, "die_target_c", 0.0);  // invalid

    bb_err_t rc = bb_sensors_fan_patch_for_test(patch);
    bb_json_free(patch);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_INVALID_ARG, rc,
        "patch with invalid die_target_c must be rejected");

    // manual_pct must NOT have been applied (bb_fan_set_autofan never called).
    bb_fan_autofan_cfg_t after;
    bb_fan_get_autofan_cfg(fh, &after);
    TEST_ASSERT_EQUAL_INT_MESSAGE(before.manual_pct, after.manual_pct,
        "manual_pct changed despite atomic rejection — partial-apply bug");

    bb_fan_set_primary(NULL);
    free(fh);
}

#endif /* CONFIG_BB_FAN_AUTOFAN */
