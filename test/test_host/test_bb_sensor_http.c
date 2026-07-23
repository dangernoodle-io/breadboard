// Tests for bb_sensor_http's per-section /api/sensors/* dispatch (B1-828 PR-2,
// FULL BREAK of the old composite bb_response-backed endpoint -- see
// bb_http_section.h for the registry-agnostic dispatch contract). Two
// layers:
//   - unit: ONE wire-copy proof each for bb_sensor_http_{fan,power,thermal}_gather()/
//     bb_sensor_http_fan_apply() against a fake fan/power backend + bb_temp_test --
//     domain-branch logic (no-primary handling, validation, the capability-gap
//     contract) now lives in bb_sensor (components/bb_sensor) and is exercised
//     exhaustively by test_bb_sensor.c; no parallel-mirror of that logic here.
//   - end-to-end: bb_sensor_http_bind_and_register() + bb_http_section_find()'s
//     render()/apply() hooks driven directly (no real HTTP server on host --
//     same pattern as test_bb_http_section.c's own e2e proof), confirming
//     the wiring is real and status-mapping is correct.
#include "unity.h"

#include "../../../components/bb_sensor_http/bb_sensor_http_wire_priv.h"
#include "../../../components/bb_sensor_http/bb_sensor_http_dispatch_priv.h"

#include "bb_data.h"
#include "bb_http_section_priv.h"
#include "bb_http_section_status.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"

#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include "bb_sensor_test.h"
#include "bb_temp.h"
#include "bb_temp_test.h"
#include "bb_json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fake fan backend
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
// Shared reset
// ---------------------------------------------------------------------------

static void sensors_test_reset(void)
{
    bb_sensor_reset_for_test();
    bb_data_test_reset();
    bb_http_section_test_reset();
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_register_format());
    memset(&g_fan, 0, sizeof(g_fan));
    memset(&g_pwr, 0, sizeof(g_pwr));
}

// ===========================================================================
// Unit: ONE wire-copy proof each for bb_sensor_http_power_gather /
// bb_sensor_http_thermal_gather -- domain-branch coverage (no-primary,
// per-source present/absent) now lives in test_bb_sensor.c.
// ===========================================================================

void test_bb_sensor_http_power_gather_with_primary_present(void)
{
    sensors_test_reset();

    bb_power_handle_t ph;
    g_pwr.vout_mv = 1200; g_pwr.iout_ma = 500; g_pwr.vin_mv = 12000; g_pwr.temp_c = 45;
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_sensor_http_power_wire_t w;
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_power_gather(&w, NULL));
    TEST_ASSERT_TRUE(w.present);
    TEST_ASSERT_EQUAL_INT64(1200, w.vout_mv);
    TEST_ASSERT_EQUAL_INT64(500, w.iout_ma);
    TEST_ASSERT_EQUAL_INT64(12000, w.vin_mv);
    TEST_ASSERT_EQUAL_INT64(45, w.temp_c);
    // pout_mw = (1200*500)/1000 = 600, per bb_power_snapshot's own SSOT calc.
    TEST_ASSERT_EQUAL_INT64(600, w.pout_mw);

    bb_power_set_primary(NULL);
    free(ph);
}

void test_bb_sensor_http_thermal_gather_soc_present_when_available(void)
{
    sensors_test_reset();
    bb_temp_test_set_soc(true, 62.5f);

    bb_sensor_http_thermal_wire_t w;
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_thermal_gather(&w, NULL));
    TEST_ASSERT_TRUE(w.soc.present);
    TEST_ASSERT_EQUAL_FLOAT(62.5f, (float)w.soc.c);
}

// ===========================================================================
// Unit: ONE wire-copy proof each for bb_sensor_http_fan_gather /
// bb_sensor_http_fan_apply (autofan build -- the non-autofan #else shape
// has no dedicated unit test here: its wire pass-through is covered by the
// ungated e2e tests below, and its capability-gap domain logic now lives in
// test_bb_sensor.c). Domain-branch coverage (no-primary handling,
// validation) also lives in test_bb_sensor.c -- no parallel-mirror here.
// ===========================================================================

#ifdef CONFIG_BB_FAN_AUTOFAN

static bb_fan_handle_t make_autofan_handle(void)
{
    bb_fan_handle_t fh;
    g_fan.die_fail = g_fan.board_fail = true;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);
    return fh;
}

void test_bb_sensor_http_fan_gather_reads_live_autofan_cfg(void)
{
    sensors_test_reset();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_fan_autofan_cfg_t cfg = { .enabled = true, .die_target_c = 65.0f,
                                  .aux_target_c = 70.0f, .min_pct = 20, .manual_pct = 80 };
    bb_fan_set_autofan(fh, &cfg);

    bb_sensor_http_fan_wire_t w;
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_fan_gather(&w, NULL));
    TEST_ASSERT_TRUE(w.autofan);
    TEST_ASSERT_EQUAL_FLOAT(65.0f, (float)w.die_target_c);
    TEST_ASSERT_EQUAL_FLOAT(70.0f, (float)w.vr_target_c);
    TEST_ASSERT_EQUAL_INT64(20, w.min_pct);
    TEST_ASSERT_EQUAL_INT64(80, w.manual_pct);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_http_fan_apply_valid_sets_autofan_cfg(void)
{
    sensors_test_reset();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_sensor_http_fan_wire_t w = { .autofan = true, .die_target_c = 62.0, .vr_target_c = 71.0,
                                 .manual_pct = 33, .min_pct = 12 };
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_fan_apply(&w, NULL));

    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(fh, &cfg);
    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_FLOAT(62.0f, cfg.die_target_c);
    TEST_ASSERT_EQUAL_FLOAT(71.0f, cfg.aux_target_c);
    TEST_ASSERT_EQUAL_INT(33, cfg.manual_pct);
    TEST_ASSERT_EQUAL_INT(12, cfg.min_pct);

    bb_fan_set_primary(NULL);
    free(fh);
}

#endif /* CONFIG_BB_FAN_AUTOFAN */

// ===========================================================================
// End-to-end: bb_sensor_http_bind_and_register() + bb_http_section_find() ->
// render()/apply() driven directly (no real HTTP server on host).
// ===========================================================================

void test_bb_sensor_http_bind_and_register_ok(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());
}

void test_bb_sensor_http_e2e_get_power_renders_json(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());

    bb_power_handle_t ph;
    g_pwr.vout_mv = 1200; g_pwr.iout_ma = 500; g_pwr.vin_mv = 12000; g_pwr.temp_c = 45;
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns =
        bb_http_section_find("/api/sensors/power", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_NOT_NULL(ns->render);
    TEST_ASSERT_EQUAL_STRING("power", name);

    char   buf[256];
    size_t out_len = 0;
    bb_err_t rc = ns->render(name, NULL, buf, sizeof(buf), &out_len, ns->ctx);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    buf[out_len] = '\0';
    TEST_ASSERT_EQUAL(200, bb_http_section_status_for_render(rc));

    bb_json_t parsed = bb_json_parse(buf, out_len);
    TEST_ASSERT_NOT_NULL(parsed);
    bool present = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "present", &present));
    TEST_ASSERT_TRUE(present);
    double vout = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "vout_mv", &vout));
    TEST_ASSERT_EQUAL_INT(1200, (int)vout);
    bb_json_free(parsed);

    bb_power_set_primary(NULL);
    free(ph);
}

void test_bb_sensor_http_e2e_power_patch_unsupported_maps_405(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns =
        bb_http_section_find("/api/sensors/power", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(ns);
    // power's bb_data binding has no apply hook; the shared sensors_apply()
    // adapter still exists on the ns, so bb_data_parse() itself is what
    // reports the apply-less binding as BB_ERR_UNSUPPORTED (PARSE stage).
    TEST_ASSERT_NOT_NULL(ns->apply);

    const char *body = "{\"vout_mv\":1200}";
    bb_http_section_apply_result_t result = ns->apply(name, body, strlen(body), ns->ctx);
    TEST_ASSERT_EQUAL(405, bb_http_section_status_for_apply(result, ns->unsupported_status));
}

void test_bb_sensor_http_e2e_thermal_get_renders_nested_sources(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());
    bb_temp_test_set_soc(true, 50.0f);

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns =
        bb_http_section_find("/api/sensors/thermal", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(ns);

    char   buf[256];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, ns->render(name, NULL, buf, sizeof(buf), &out_len, ns->ctx));
    buf[out_len] = '\0';

    bb_json_t parsed = bb_json_parse(buf, out_len);
    TEST_ASSERT_NOT_NULL(parsed);
    bb_json_t soc = bb_json_obj_get_item(parsed, "soc");
    TEST_ASSERT_NOT_NULL(soc);
    bool present = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(soc, "present", &present));
    TEST_ASSERT_TRUE(present);
    double c = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(soc, "c", &c));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, (float)c);
    bb_json_free(parsed);
}

void test_bb_sensor_http_e2e_get_fan_no_primary_returns_200_present_false(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns =
        bb_http_section_find("/api/sensors/fan", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_NOT_NULL(ns->render);

    char   buf[256];
    size_t out_len = 0;
    bb_err_t rc = ns->render(name, NULL, buf, sizeof(buf), &out_len, ns->ctx);
    // Regression pin: reverting bb_sensor_http_fan_gather()'s no-primary branch
    // back to "return BB_ERR_INVALID_STATE" turns this red (500 instead of
    // 200) -- see the RED capture in the PR report.
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, bb_http_section_status_for_render(rc));

    buf[out_len] = '\0';
    bb_json_t parsed = bb_json_parse(buf, out_len);
    TEST_ASSERT_NOT_NULL(parsed);
    bool present = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "present", &present));
    TEST_ASSERT_FALSE(present);
    bb_json_free(parsed);
}

void test_bb_sensor_http_e2e_fan_patch_no_primary_maps_503(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns =
        bb_http_section_find("/api/sensors/fan", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_NOT_NULL(ns->apply);

    const char *body = "{}";
    bb_http_section_apply_result_t result = ns->apply(name, body, strlen(body), ns->ctx);
    TEST_ASSERT_EQUAL(503, bb_http_section_status_for_apply(result, ns->unsupported_status));
}

void test_bb_sensor_http_e2e_unbound_key_returns_404_never_reaches_bb_data(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());

    // Bind a real, unrelated key -- the exact hazard FIX 1 closes: without
    // the fixed-set check, /api/sensors/log would proxy straight through to
    // this binding (bb_data_bind() is a single flat, process-wide table
    // with no namespace concept).
    bb_data_binding_t log_binding = {
        .key    = "log",
        .desc   = &bb_sensor_http_power_wire_desc,
        .gather = bb_sensor_http_power_gather,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&log_binding));

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns =
        bb_http_section_find("/api/sensors/log", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_EQUAL_STRING("log", name);

    char   buf[256];
    size_t out_len = 0;
    bb_err_t rc = ns->render(name, NULL, buf, sizeof(buf), &out_len, ns->ctx);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, rc);
    TEST_ASSERT_EQUAL(404, bb_http_section_status_for_render(rc));

    const char *body = "{}";
    bb_http_section_apply_result_t result = ns->apply(name, body, strlen(body), ns->ctx);
    TEST_ASSERT_EQUAL(404, bb_http_section_status_for_apply(result, ns->unsupported_status));
}

#ifdef CONFIG_BB_FAN_AUTOFAN
void test_bb_sensor_http_e2e_fan_patch_applies_and_validates(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());
    bb_fan_handle_t fh = make_autofan_handle();

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns =
        bb_http_section_find("/api/sensors/fan", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_NOT_NULL(ns->apply);

    // Partial PATCH: only manual_pct supplied -- PATCH-mode seeds the rest
    // from the live cfg (bb_sensor_http_fan_gather()'s "real seed"), so
    // die_target_c/vr_target_c/min_pct/autofan stay at their current values.
    const char *body = "{\"manual_pct\":77}";
    bb_http_section_apply_result_t result = ns->apply(name, body, strlen(body), ns->ctx);
    TEST_ASSERT_EQUAL(200, bb_http_section_status_for_apply(result, ns->unsupported_status));

    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(fh, &cfg);
    TEST_ASSERT_EQUAL_INT(77, cfg.manual_pct);

    // A malformed body maps 400 (PARSE stage).
    const char *bad_body = "{not json";
    result = ns->apply(name, bad_body, strlen(bad_body), ns->ctx);
    TEST_ASSERT_EQUAL(400, bb_http_section_status_for_apply(result, ns->unsupported_status));

    // An out-of-range field maps 400 (COMMIT stage, BB_ERR_VALIDATION).
    const char *invalid_body = "{\"min_pct\":500}";
    result = ns->apply(name, invalid_body, strlen(invalid_body), ns->ctx);
    TEST_ASSERT_EQUAL(400, bb_http_section_status_for_apply(result, ns->unsupported_status));

    bb_fan_set_primary(NULL);
    free(fh);
}

// B1-1164: a fraction/exponent-form "manual_pct" (e.g. 5e1, the JSON
// spelling of 50) used to silently truncate through strtoll() into the
// wrong integer -- PATCH {"manual_pct":5e1} landed a fan duty of 5, not 50,
// with no error. bb_serialize_json_tok_get_i64() now refuses that token
// outright, which BB_TYPE_I64's populate arm (bb_serialize_populate.c)
// already treats identically to "field absent" (same false-return
// contract every getter shares) -- so the corrupt value is never applied:
// PATCH-mode's gather() seed keeps manual_pct at its live pre-PATCH value,
// still 200 (a fraction/exponent numeral is not, on its own, a malformed
// JSON body), rather than silently landing 5.
void test_bb_sensor_http_e2e_fan_patch_exponent_manual_pct_refused_not_truncated(void)
{
    sensors_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_bind_and_register());
    bb_fan_handle_t fh = make_autofan_handle();

    bb_fan_autofan_cfg_t seed_cfg = { .enabled = true, .die_target_c = 65.0f,
                                       .aux_target_c = 70.0f, .min_pct = 20, .manual_pct = 42 };
    bb_fan_set_autofan(fh, &seed_cfg);

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns =
        bb_http_section_find("/api/sensors/fan", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_NOT_NULL(ns->apply);

    const char *body = "{\"manual_pct\":5e1}";
    bb_http_section_apply_result_t result = ns->apply(name, body, strlen(body), ns->ctx);
    TEST_ASSERT_EQUAL(200, bb_http_section_status_for_apply(result, ns->unsupported_status));

    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(fh, &cfg);
    TEST_ASSERT_EQUAL_INT(42, cfg.manual_pct);  // unchanged -- refused, not truncated to 5
    TEST_ASSERT_NOT_EQUAL(50, cfg.manual_pct);  // and never silently corrupted to the "intended" 50 either

    bb_fan_set_primary(NULL);
    free(fh);
}
#endif /* CONFIG_BB_FAN_AUTOFAN */
