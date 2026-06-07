// Tests for /api/power route: handler emit + extender integration + schema.
//
// The power route handler on espidf uses bb_json_t (tree-based). On host,
// we exercise the same semantics via a local h_power that uses the tree
// JSON API (same as espidf handler), so extenders (which take bb_json_t) work.
// Extender integration uses bb_http_route_run_extenders + bb_json_t directly.
#include "unity.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include "bb_power_routes.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "bb_http_extender.h"
#include "bb_http_extender_test.h"
#include "bb_json.h"
#include "bb_info.h"
#include "bb_info_test.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Fake power backend
// ---------------------------------------------------------------------------

typedef struct {
    int vout_mv, iout_ma, vin_mv, temp_c;
} fake_pwr_state_t;

static fake_pwr_state_t g_pwr;

static int fp_vout(void *s) { return ((fake_pwr_state_t *)s)->vout_mv; }
static int fp_iout(void *s) { return ((fake_pwr_state_t *)s)->iout_ma; }
static int fp_vin (void *s) { return ((fake_pwr_state_t *)s)->vin_mv; }
static int fp_temp(void *s) { return ((fake_pwr_state_t *)s)->temp_c; }
static bb_err_t fp_set(void *s, uint16_t mv) { (void)s; (void)mv; return BB_OK; }

static const bb_power_driver_t drv_fake = {
    .read_vout_mv = fp_vout,
    .read_iout_ma = fp_iout,
    .read_vin_mv  = fp_vin,
    .read_temp_c  = fp_temp,
    .set_vout_mv  = fp_set,
    .name         = "test_pwr",
};

// ---------------------------------------------------------------------------
// Local handler implementation (streaming JSON, mirrors espidf handler logic)
// Exercises the same JSON shape as the production handler.
// ---------------------------------------------------------------------------

static bb_err_t h_power(bb_http_request_t *req)
{
    bb_power_handle_t h = bb_power_primary();
    bool present = (h != NULL);

    bb_power_snapshot_t snap;
    bb_power_snapshot(h, &snap);

    // Build a bb_json_t tree so extenders (which take bb_json_t) can add fields.
    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_bool(root, "present", present);

    if (present && snap.vout_mv >= 0) {
        bb_json_obj_set_number(root, "vout_mv", (double)snap.vout_mv);
    } else {
        bb_json_obj_set_null(root, "vout_mv");
    }
    if (present && snap.iout_ma >= 0) {
        bb_json_obj_set_number(root, "iout_ma", (double)snap.iout_ma);
    } else {
        bb_json_obj_set_null(root, "iout_ma");
    }
    if (present && snap.pout_mw >= 0) {
        bb_json_obj_set_number(root, "pout_mw", (double)snap.pout_mw);
    } else {
        bb_json_obj_set_null(root, "pout_mw");
    }
    if (present && snap.vin_mv >= 0) {
        bb_json_obj_set_number(root, "vin_mv", (double)snap.vin_mv);
    } else {
        bb_json_obj_set_null(root, "vin_mv");
    }
    if (present && snap.temp_c >= 0) {
        bb_json_obj_set_number(root, "temp_c", (double)snap.temp_c);
    } else {
        bb_json_obj_set_null(root, "temp_c");
    }

    bb_http_route_run_extenders("power", root);

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
// Helper: run h_power through host capture, parse result
// ---------------------------------------------------------------------------

static cJSON *run_h_power(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    h_power(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    cJSON *parsed = NULL;
    if (cap.body) parsed = cJSON_Parse(cap.body);
    bb_http_host_capture_free(&cap);
    return parsed;
}

// ---------------------------------------------------------------------------
// Tests: present:true — all fields emitted
// ---------------------------------------------------------------------------

void test_bb_power_routes_present_true_fields(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_fake, &g_pwr, &h);

    g_pwr.vout_mv = 1200;
    g_pwr.iout_ma = 5000;
    g_pwr.vin_mv  = 12000;
    g_pwr.temp_c  = 60;
    bb_power_poll(h);
    bb_power_set_primary(h);

    cJSON *body = run_h_power();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "handler did not emit valid JSON");

    cJSON *present = cJSON_GetObjectItemCaseSensitive(body, "present");
    TEST_ASSERT_NOT_NULL_MESSAGE(present, "missing 'present'");
    TEST_ASSERT_TRUE(cJSON_IsTrue(present));

    cJSON *vout = cJSON_GetObjectItemCaseSensitive(body, "vout_mv");
    TEST_ASSERT_NOT_NULL(vout);
    TEST_ASSERT_EQUAL_INT(1200, (int)cJSON_GetNumberValue(vout));

    cJSON *iout = cJSON_GetObjectItemCaseSensitive(body, "iout_ma");
    TEST_ASSERT_NOT_NULL(iout);
    TEST_ASSERT_EQUAL_INT(5000, (int)cJSON_GetNumberValue(iout));

    cJSON *pout = cJSON_GetObjectItemCaseSensitive(body, "pout_mw");
    TEST_ASSERT_NOT_NULL(pout);
    TEST_ASSERT_EQUAL_INT(6000, (int)cJSON_GetNumberValue(pout));

    cJSON *vin = cJSON_GetObjectItemCaseSensitive(body, "vin_mv");
    TEST_ASSERT_NOT_NULL(vin);
    TEST_ASSERT_EQUAL_INT(12000, (int)cJSON_GetNumberValue(vin));

    cJSON *temp = cJSON_GetObjectItemCaseSensitive(body, "temp_c");
    TEST_ASSERT_NOT_NULL(temp);
    TEST_ASSERT_EQUAL_INT(60, (int)cJSON_GetNumberValue(temp));

    cJSON_Delete(body);
    bb_power_set_primary(NULL);
    free(h);
}

// ---------------------------------------------------------------------------
// Tests: present:false — no primary, all numeric fields are null
// ---------------------------------------------------------------------------

void test_bb_power_routes_no_primary_present_false(void)
{
    bb_power_set_primary(NULL);

    cJSON *body = run_h_power();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "handler did not emit valid JSON");

    cJSON *present = cJSON_GetObjectItemCaseSensitive(body, "present");
    TEST_ASSERT_NOT_NULL_MESSAGE(present, "missing 'present'");
    TEST_ASSERT_FALSE(cJSON_IsTrue(present));

    // All numeric fields should be null
    cJSON *vout = cJSON_GetObjectItemCaseSensitive(body, "vout_mv");
    TEST_ASSERT_NOT_NULL(vout);
    TEST_ASSERT_TRUE(cJSON_IsNull(vout));

    cJSON *iout = cJSON_GetObjectItemCaseSensitive(body, "iout_ma");
    TEST_ASSERT_NOT_NULL(iout);
    TEST_ASSERT_TRUE(cJSON_IsNull(iout));

    cJSON *pout = cJSON_GetObjectItemCaseSensitive(body, "pout_mw");
    TEST_ASSERT_NOT_NULL(pout);
    TEST_ASSERT_TRUE(cJSON_IsNull(pout));

    cJSON *vin = cJSON_GetObjectItemCaseSensitive(body, "vin_mv");
    TEST_ASSERT_NOT_NULL(vin);
    TEST_ASSERT_TRUE(cJSON_IsNull(vin));

    cJSON *temp = cJSON_GetObjectItemCaseSensitive(body, "temp_c");
    TEST_ASSERT_NOT_NULL(temp);
    TEST_ASSERT_TRUE(cJSON_IsNull(temp));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// Tests: error readings → null
// ---------------------------------------------------------------------------

static int always_err(void *s) { (void)s; return -1; }
static const bb_power_driver_t drv_err = {
    .read_vout_mv = always_err,
    .read_iout_ma = always_err,
    .read_vin_mv  = always_err,
    .read_temp_c  = always_err,
    .set_vout_mv  = fp_set,
    .name         = "err_pwr",
};

void test_bb_power_routes_error_readings_emit_null(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_err, &g_pwr, &h);
    bb_power_poll(h);
    bb_power_set_primary(h);

    cJSON *body = run_h_power();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *present = cJSON_GetObjectItemCaseSensitive(body, "present");
    TEST_ASSERT_TRUE(cJSON_IsTrue(present));

    cJSON *vout = cJSON_GetObjectItemCaseSensitive(body, "vout_mv");
    TEST_ASSERT_TRUE(cJSON_IsNull(vout));

    cJSON *iout = cJSON_GetObjectItemCaseSensitive(body, "iout_ma");
    TEST_ASSERT_TRUE(cJSON_IsNull(iout));

    cJSON_Delete(body);
    bb_power_set_primary(NULL);
    free(h);
}

// ---------------------------------------------------------------------------
// Tests: extender fields appear in output
// ---------------------------------------------------------------------------

static void power_extender_add_extra(void *root)
{
    bb_json_obj_set_number(root, "pcore_mw", 42000.0);
}

void test_bb_power_routes_extender_field_appears(void)
{
    bb_http_register_route_extender("power",
                                    (bb_http_extender_fn)power_extender_add_extra,
                                    NULL);
    bb_power_set_primary(NULL);

    cJSON *body = run_h_power();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *extra = cJSON_GetObjectItemCaseSensitive(body, "pcore_mw");
    TEST_ASSERT_NOT_NULL_MESSAGE(extra, "extender field 'pcore_mw' missing");
    TEST_ASSERT_EQUAL_INT(42000, (int)cJSON_GetNumberValue(extra));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// Tests: assembled schema contains base fields + extender fragment
// ---------------------------------------------------------------------------

void test_bb_power_routes_schema_contains_base_fields(void)
{
    const char *schema = bb_power_routes_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"present\""), "present not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"vout_mv\""), "vout_mv not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"iout_ma\""), "iout_ma not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"pout_mw\""), "pout_mw not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"vin_mv\""),  "vin_mv not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"temp_c\""),  "temp_c not in schema");
}

void test_bb_power_routes_schema_contains_extender_fragment(void)
{
    static const char frag[] = "\"pcore_mw\":{\"type\":\"integer\"}";
    bb_http_register_route_extender("power",
                                    (bb_http_extender_fn)power_extender_add_extra,
                                    frag);
    const char *schema = bb_power_routes_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag),
        "extender fragment not found in assembled schema");
}
