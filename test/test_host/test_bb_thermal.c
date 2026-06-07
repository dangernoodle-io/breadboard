// Tests for /api/thermal route: GET handler + extender integration + schema.
//
// Sources aggregated:
//   soc:   bb_temp_read_soc (injectable via BB_TEMP_TESTING)
//   vr:    bb_power_primary() + bb_power_snapshot() → .temp_c
//   asic:  bb_fan_primary() + bb_fan_snapshot() → .die_c
//   board: bb_fan_primary() + bb_fan_snapshot() → .board_c
#include "unity.h"
#include "bb_thermal.h"
#include "bb_temp.h"
#include "bb_temp_test.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "bb_http_extender.h"
#include "bb_http_extender_test.h"
#include "bb_json.h"
#include "bb_info.h"
#include "bb_info_test.h"
#include "cJSON.h"

#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

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
    .name         = "test_pwr",
};

// ---------------------------------------------------------------------------
// Fake fan backend
// ---------------------------------------------------------------------------

typedef struct {
    int rpm;
    int duty_pct;
    float die_c;
    float board_c;
    bool die_fail;
    bool board_fail;
} fake_fan_t;

static fake_fan_t g_fan;

static bb_err_t ff_set_duty(void *s, int pct) { (void)s; (void)pct; return BB_OK; }
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
// Local GET handler (mirrors espidf handler)
// ---------------------------------------------------------------------------

static bb_err_t h_thermal_get(bb_http_request_t *req)
{
    // SoC
    float soc_c = 0.0f;
    bool soc_present = bb_temp_read_soc(&soc_c);

    // VR
    bb_power_handle_t pwr = bb_power_primary();
    bb_power_snapshot_t psnap;
    bb_power_snapshot(pwr, &psnap);
    bool vr_present = (pwr != NULL && psnap.temp_c >= 0);

    // Fan: ASIC die + board
    bb_fan_handle_t fan = bb_fan_primary();
    bb_fan_snapshot_t fsnap;
    bb_fan_snapshot(fan, &fsnap);
    bool asic_present  = (fan != NULL && !isnan(fsnap.die_c));
    bool board_present = (fan != NULL && !isnan(fsnap.board_c));

    bb_json_t root = bb_json_obj_new();

    // soc
    bb_json_t soc_obj = bb_json_obj_new();
    bb_json_obj_set_bool(soc_obj, "present", soc_present);
    if (soc_present) {
        bb_json_obj_set_number(soc_obj, "c", (double)soc_c);
    } else {
        bb_json_obj_set_null(soc_obj, "c");
    }
    bb_json_obj_set_obj(root, "soc", soc_obj);

    // vr
    bb_json_t vr_obj = bb_json_obj_new();
    bb_json_obj_set_bool(vr_obj, "present", vr_present);
    if (vr_present) {
        bb_json_obj_set_number(vr_obj, "c", (double)psnap.temp_c);
    } else {
        bb_json_obj_set_null(vr_obj, "c");
    }
    bb_json_obj_set_obj(root, "vr", vr_obj);

    // asic
    bb_json_t asic_obj = bb_json_obj_new();
    bb_json_obj_set_bool(asic_obj, "present", asic_present);
    if (asic_present) {
        bb_json_obj_set_number(asic_obj, "c", (double)fsnap.die_c);
    } else {
        bb_json_obj_set_null(asic_obj, "c");
    }
    bb_json_obj_set_obj(root, "asic", asic_obj);

    // board
    bb_json_t board_obj = bb_json_obj_new();
    bb_json_obj_set_bool(board_obj, "present", board_present);
    if (board_present) {
        bb_json_obj_set_number(board_obj, "c", (double)fsnap.board_c);
    } else {
        bb_json_obj_set_null(board_obj, "c");
    }
    bb_json_obj_set_obj(root, "board", board_obj);

    bb_http_route_run_extenders("thermal", root);

    char *str = bb_json_serialize(root);
    bb_json_free(root);
    if (!str) {
        bb_http_resp_send_chunk(req, NULL, 0);
        return BB_ERR_NO_SPACE;
    }
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send_chunk(req, str, -1);
    bb_http_resp_send_chunk(req, NULL, 0);
    free(str);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Helper: run handler and parse result
// ---------------------------------------------------------------------------

static cJSON *run_get(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    h_thermal_get(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    cJSON *parsed = NULL;
    if (cap.body) parsed = cJSON_Parse(cap.body);
    bb_http_host_capture_free(&cap);
    return parsed;
}

// ---------------------------------------------------------------------------
// all-present: soc + vr + asic + board all present with correct c values
// ---------------------------------------------------------------------------

void test_bb_thermal_all_present(void)
{
    // soc
    bb_temp_test_set_soc(true, 55.0f);

    // vr
    bb_power_handle_t ph;
    g_pwr.temp_c = 60;
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    // fan
    bb_fan_handle_t fh;
    g_fan.die_c   = 72.5f;
    g_fan.board_c = 45.0f;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "handler did not emit valid JSON");

    // soc
    cJSON *soc = cJSON_GetObjectItemCaseSensitive(body, "soc");
    TEST_ASSERT_NOT_NULL(soc);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(soc, "present")));
    cJSON *soc_c = cJSON_GetObjectItemCaseSensitive(soc, "c");
    TEST_ASSERT_NOT_NULL(soc_c);
    TEST_ASSERT_FALSE(cJSON_IsNull(soc_c));
    TEST_ASSERT_EQUAL_INT(55, (int)cJSON_GetNumberValue(soc_c));

    // vr
    cJSON *vr = cJSON_GetObjectItemCaseSensitive(body, "vr");
    TEST_ASSERT_NOT_NULL(vr);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(vr, "present")));
    cJSON *vr_c = cJSON_GetObjectItemCaseSensitive(vr, "c");
    TEST_ASSERT_NOT_NULL(vr_c);
    TEST_ASSERT_FALSE(cJSON_IsNull(vr_c));
    TEST_ASSERT_EQUAL_INT(60, (int)cJSON_GetNumberValue(vr_c));

    // asic
    cJSON *asic = cJSON_GetObjectItemCaseSensitive(body, "asic");
    TEST_ASSERT_NOT_NULL(asic);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(asic, "present")));
    cJSON *asic_c = cJSON_GetObjectItemCaseSensitive(asic, "c");
    TEST_ASSERT_NOT_NULL(asic_c);
    TEST_ASSERT_FALSE(cJSON_IsNull(asic_c));
    // 72.5 rounds to 72 as int
    TEST_ASSERT_EQUAL_INT(72, (int)cJSON_GetNumberValue(asic_c));

    // board
    cJSON *board = cJSON_GetObjectItemCaseSensitive(body, "board");
    TEST_ASSERT_NOT_NULL(board);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(board, "present")));
    cJSON *board_c = cJSON_GetObjectItemCaseSensitive(board, "c");
    TEST_ASSERT_NOT_NULL(board_c);
    TEST_ASSERT_FALSE(cJSON_IsNull(board_c));
    TEST_ASSERT_EQUAL_INT(45, (int)cJSON_GetNumberValue(board_c));

    cJSON_Delete(body);
    bb_power_set_primary(NULL);
    free(ph);
    bb_fan_set_primary(NULL);
    free(fh);
}

// ---------------------------------------------------------------------------
// all-absent: no primaries, soc not present → all present:false, c:null
// ---------------------------------------------------------------------------

void test_bb_thermal_all_absent(void)
{
    // defaults from setUp: soc absent, no primaries

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "handler did not emit valid JSON");

    const char *keys[] = {"soc", "vr", "asic", "board"};
    for (int i = 0; i < 4; i++) {
        cJSON *src = cJSON_GetObjectItemCaseSensitive(body, keys[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(src, "source object missing");

        cJSON *present = cJSON_GetObjectItemCaseSensitive(src, "present");
        TEST_ASSERT_NOT_NULL(present);
        TEST_ASSERT_FALSE_MESSAGE(cJSON_IsTrue(present), "present should be false");

        cJSON *c = cJSON_GetObjectItemCaseSensitive(src, "c");
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNull(c), "c should be null when absent");
    }

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// mixed: soc present, no vr/fan → soc present, others absent
// ---------------------------------------------------------------------------

void test_bb_thermal_mixed_soc_only(void)
{
    bb_temp_test_set_soc(true, 48.0f);
    // no power or fan primary set

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "handler did not emit valid JSON");

    // soc: present
    cJSON *soc = cJSON_GetObjectItemCaseSensitive(body, "soc");
    TEST_ASSERT_NOT_NULL(soc);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(soc, "present")));
    cJSON *soc_c = cJSON_GetObjectItemCaseSensitive(soc, "c");
    TEST_ASSERT_FALSE(cJSON_IsNull(soc_c));
    TEST_ASSERT_EQUAL_INT(48, (int)cJSON_GetNumberValue(soc_c));

    // vr, asic, board: absent
    const char *absent[] = {"vr", "asic", "board"};
    for (int i = 0; i < 3; i++) {
        cJSON *src = cJSON_GetObjectItemCaseSensitive(body, absent[i]);
        TEST_ASSERT_NOT_NULL(src);
        TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(src, "present")));
        TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(src, "c")));
    }

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// mixed: vr present, soc/asic/board absent
// ---------------------------------------------------------------------------

void test_bb_thermal_mixed_vr_only(void)
{
    // soc absent (default)
    bb_power_handle_t ph;
    g_pwr.temp_c = 75;
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);
    // no fan primary

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "handler did not emit valid JSON");

    // soc: absent
    cJSON *soc = cJSON_GetObjectItemCaseSensitive(body, "soc");
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(soc, "present")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(soc, "c")));

    // vr: present
    cJSON *vr = cJSON_GetObjectItemCaseSensitive(body, "vr");
    TEST_ASSERT_NOT_NULL(vr);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(vr, "present")));
    cJSON *vr_c = cJSON_GetObjectItemCaseSensitive(vr, "c");
    TEST_ASSERT_FALSE(cJSON_IsNull(vr_c));
    TEST_ASSERT_EQUAL_INT(75, (int)cJSON_GetNumberValue(vr_c));

    // asic, board: absent (no fan)
    cJSON *asic = cJSON_GetObjectItemCaseSensitive(body, "asic");
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(asic, "present")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(asic, "c")));

    cJSON *board = cJSON_GetObjectItemCaseSensitive(body, "board");
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(board, "present")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(board, "c")));

    cJSON_Delete(body);
    bb_power_set_primary(NULL);
    free(ph);
}

// ---------------------------------------------------------------------------
// vr: temp_c == -1 → vr absent even though primary is set
// ---------------------------------------------------------------------------

void test_bb_thermal_vr_temp_minus1_absent(void)
{
    bb_power_handle_t ph;
    g_pwr.temp_c = -1;  // unavailable
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *vr = cJSON_GetObjectItemCaseSensitive(body, "vr");
    TEST_ASSERT_NOT_NULL(vr);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(vr, "present")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(vr, "c")));

    cJSON_Delete(body);
    bb_power_set_primary(NULL);
    free(ph);
}

// ---------------------------------------------------------------------------
// fan: die_c NAN → asic absent; board_c valid → board present
// ---------------------------------------------------------------------------

void test_bb_thermal_asic_nan_board_present(void)
{
    bb_fan_handle_t fh;
    g_fan.die_fail   = true;   // die_c will be NAN after poll
    g_fan.board_c    = 40.0f;
    g_fan.board_fail = false;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    // asic absent (die_c NAN)
    cJSON *asic = cJSON_GetObjectItemCaseSensitive(body, "asic");
    TEST_ASSERT_NOT_NULL(asic);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(asic, "present")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(asic, "c")));

    // board present
    cJSON *board = cJSON_GetObjectItemCaseSensitive(body, "board");
    TEST_ASSERT_NOT_NULL(board);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(board, "present")));
    cJSON *board_c = cJSON_GetObjectItemCaseSensitive(board, "c");
    TEST_ASSERT_FALSE(cJSON_IsNull(board_c));
    TEST_ASSERT_EQUAL_INT(40, (int)cJSON_GetNumberValue(board_c));

    cJSON_Delete(body);
    bb_fan_set_primary(NULL);
    free(fh);
}

// ---------------------------------------------------------------------------
// extender: a registered "thermal" extender's field appears in output
// ---------------------------------------------------------------------------

static void thermal_extender_add_mining_c(void *root)
{
    bb_json_obj_set_number(root, "mining_c", 88.0);
}

void test_bb_thermal_extender_field_appears(void)
{
    bb_http_register_route_extender("thermal",
                                    (bb_http_extender_fn)thermal_extender_add_mining_c,
                                    NULL);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *extra = cJSON_GetObjectItemCaseSensitive(body, "mining_c");
    TEST_ASSERT_NOT_NULL_MESSAGE(extra, "extender field 'mining_c' missing");
    TEST_ASSERT_EQUAL_INT(88, (int)cJSON_GetNumberValue(extra));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// schema: base four objects present
// ---------------------------------------------------------------------------

void test_bb_thermal_schema_contains_base_objects(void)
{
    const char *schema = bb_thermal_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"soc\""),   "soc not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"vr\""),    "vr not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"asic\""),  "asic not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"board\""), "board not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"present\""), "present not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"c\""),     "c not in schema");
}

// ---------------------------------------------------------------------------
// schema: extender fragment appears after registration
// ---------------------------------------------------------------------------

void test_bb_thermal_schema_contains_extender_fragment(void)
{
    static const char frag[] = "\"mining_c\":{\"type\":\"number\"}";
    bb_http_register_route_extender("thermal",
                                    (bb_http_extender_fn)thermal_extender_add_mining_c,
                                    frag);
    const char *schema = bb_thermal_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag),
        "extender fragment not found in assembled schema");
}
