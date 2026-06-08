// Tests for /api/fan route: GET handler + extender integration + POST duty + schema.
#include "unity.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include "bb_fan_routes.h"
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
#include <math.h>

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
    int set_duty_last;
    bb_err_t set_duty_ret;
} fake_fan_t;

static fake_fan_t g_fan;

static bb_err_t ff_set_duty(void *s, int pct)
{
    fake_fan_t *f = s;
    f->set_duty_last = pct;
    f->duty_pct = pct;
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

static const bb_fan_driver_t drv_fake = {
    .set_duty_pct     = ff_set_duty,
    .get_duty_pct     = ff_get_duty,
    .read_rpm         = ff_rpm,
    .read_die_temp_c  = ff_die,
    .read_board_temp_c = ff_board,
    .name             = "fake_fan",
};

// ---------------------------------------------------------------------------
// Local GET handler (mirrors espidf handler)
// ---------------------------------------------------------------------------

static bb_err_t h_fan_get(bb_http_request_t *req)
{
    bb_fan_handle_t h = bb_fan_primary();
    bool present = (h != NULL);

    bb_fan_snapshot_t snap;
    bb_fan_snapshot(h, &snap);

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_bool(root, "present", present);

    if (present && snap.rpm >= 0) {
        bb_json_obj_set_number(root, "rpm", (double)snap.rpm);
    } else {
        bb_json_obj_set_null(root, "rpm");
    }

    if (present && snap.duty_pct >= 0) {
        bb_json_obj_set_number(root, "duty_pct", (double)snap.duty_pct);
    } else {
        bb_json_obj_set_null(root, "duty_pct");
    }

    bb_http_route_run_extenders("fan", root);

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
// Local POST handler (mirrors espidf handler)
// ---------------------------------------------------------------------------

static bb_err_t h_fan_post(bb_http_request_t *req)
{
    bb_fan_handle_t h = bb_fan_primary();
    if (!h) {
        bb_http_resp_set_status(req, 503);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "no primary fan");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_STATE;
    }

    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > 256) {
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "missing or oversized body");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }

    char body[256];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "read failed");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    if (!parsed) {
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "invalid JSON");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }

    double duty_d = -1.0;
    int duty = -1;
    if (!bb_json_obj_get_number(parsed, "duty_pct", &duty_d)) {
        bb_json_free(parsed);
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "duty_pct required");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }
    duty = (int)duty_d;
    bb_json_free(parsed);

    if (duty < 0 || duty > 100) {
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "duty_pct must be 0..100");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }

    bb_fan_set_duty_pct(h, duty);

    bb_json_t resp = bb_json_obj_new();
    bb_json_obj_set_string(resp, "status", "ok");
    bb_json_obj_set_number(resp, "duty_pct", (double)duty);
    char *str = bb_json_serialize(resp);
    bb_json_free(resp);
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
// Helpers
// ---------------------------------------------------------------------------

static cJSON *run_get(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    h_fan_get(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    cJSON *parsed = NULL;
    if (cap.body) parsed = cJSON_Parse(cap.body);
    bb_http_host_capture_free(&cap);
    return parsed;
}

static bb_http_host_capture_t run_post(const char *body)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (body) bb_http_host_capture_set_req_body(body, (int)strlen(body));
    h_fan_post(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    return cap;
}

// ---------------------------------------------------------------------------
// GET: present:true
// ---------------------------------------------------------------------------

void test_bb_fan_routes_get_present_true(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);

    g_fan.rpm      = 3000;
    g_fan.duty_pct = 80;
    bb_fan_poll(h);
    bb_fan_set_primary(h);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "handler did not emit valid JSON");

    cJSON *present = cJSON_GetObjectItemCaseSensitive(body, "present");
    TEST_ASSERT_NOT_NULL(present);
    TEST_ASSERT_TRUE(cJSON_IsTrue(present));

    cJSON *rpm = cJSON_GetObjectItemCaseSensitive(body, "rpm");
    TEST_ASSERT_NOT_NULL(rpm);
    TEST_ASSERT_EQUAL_INT(3000, (int)cJSON_GetNumberValue(rpm));

    cJSON *duty = cJSON_GetObjectItemCaseSensitive(body, "duty_pct");
    TEST_ASSERT_NOT_NULL(duty);
#ifdef CONFIG_BB_FAN_AUTOFAN
    // BB owns duty when autofan compiled in: poll applies manual_pct (default 100).
    TEST_ASSERT_EQUAL_INT(100, (int)cJSON_GetNumberValue(duty));
#else
    TEST_ASSERT_EQUAL_INT(80, (int)cJSON_GetNumberValue(duty));
#endif

    cJSON_Delete(body);
    bb_fan_set_primary(NULL);
    free(h);
}

// ---------------------------------------------------------------------------
// GET: present:false
// ---------------------------------------------------------------------------

void test_bb_fan_routes_get_no_primary_present_false(void)
{
    bb_fan_set_primary(NULL);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "handler did not emit valid JSON");

    cJSON *present = cJSON_GetObjectItemCaseSensitive(body, "present");
    TEST_ASSERT_NOT_NULL(present);
    TEST_ASSERT_FALSE(cJSON_IsTrue(present));

    cJSON *rpm = cJSON_GetObjectItemCaseSensitive(body, "rpm");
    TEST_ASSERT_NOT_NULL(rpm);
    TEST_ASSERT_TRUE(cJSON_IsNull(rpm));

    cJSON *duty = cJSON_GetObjectItemCaseSensitive(body, "duty_pct");
    TEST_ASSERT_NOT_NULL(duty);
    TEST_ASSERT_TRUE(cJSON_IsNull(duty));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET: error readings → null
// ---------------------------------------------------------------------------

static int always_minus1(void *s) { (void)s; return -1; }
static bb_err_t always_err_die(void *s, float *out) { (void)s; (void)out; return BB_ERR_INVALID_STATE; }
static bb_err_t always_err_board(void *s, float *out) { (void)s; (void)out; return BB_ERR_INVALID_STATE; }
static bb_err_t noop_set(void *s, int p) { (void)s; (void)p; return BB_OK; }
static int always_minus1_get(void *s) { (void)s; return -1; }

static const bb_fan_driver_t drv_err_reads = {
    .set_duty_pct     = noop_set,
    .get_duty_pct     = always_minus1_get,
    .read_rpm         = always_minus1,
    .read_die_temp_c  = always_err_die,
    .read_board_temp_c = always_err_board,
    .name             = "err_fan",
};

void test_bb_fan_routes_get_error_readings_emit_null(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_err_reads, &g_fan, &h);
    bb_fan_poll(h);
    bb_fan_set_primary(h);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *present = cJSON_GetObjectItemCaseSensitive(body, "present");
    TEST_ASSERT_TRUE(cJSON_IsTrue(present));

    cJSON *rpm = cJSON_GetObjectItemCaseSensitive(body, "rpm");
    TEST_ASSERT_TRUE(cJSON_IsNull(rpm));

    cJSON *duty = cJSON_GetObjectItemCaseSensitive(body, "duty_pct");
#ifdef CONFIG_BB_FAN_AUTOFAN
    // When autofan compiled in, BB owns duty: poll applies manual_pct (default 100).
    TEST_ASSERT_FALSE(cJSON_IsNull(duty));
    TEST_ASSERT_EQUAL_INT(100, (int)cJSON_GetNumberValue(duty));
#else
    TEST_ASSERT_TRUE(cJSON_IsNull(duty));
#endif

    cJSON_Delete(body);
    bb_fan_set_primary(NULL);
    free(h);
}

// ---------------------------------------------------------------------------
// GET: extender field appears
// ---------------------------------------------------------------------------

static void fan_extender_add_extra(void *root)
{
    bb_json_obj_set_number(root, "autofan_target_c", 70.0);
}

void test_bb_fan_routes_get_extender_field_appears(void)
{
    bb_http_register_route_extender("fan",
                                    (bb_http_extender_fn)fan_extender_add_extra,
                                    NULL);
    bb_fan_set_primary(NULL);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *extra = cJSON_GetObjectItemCaseSensitive(body, "autofan_target_c");
    TEST_ASSERT_NOT_NULL_MESSAGE(extra, "extender field 'autofan_target_c' missing");
    TEST_ASSERT_EQUAL_INT(70, (int)cJSON_GetNumberValue(extra));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// POST: valid duty
// ---------------------------------------------------------------------------

void test_bb_fan_routes_post_valid_sets_duty(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);
    bb_fan_set_primary(h);

    bb_http_host_capture_t cap = run_post("{\"duty_pct\":50}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);

    cJSON *body = NULL;
    if (cap.body) body = cJSON_Parse(cap.body);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_NOT_NULL_MESSAGE(body, "POST did not return JSON");
    cJSON *status = cJSON_GetObjectItemCaseSensitive(body, "status");
    TEST_ASSERT_NOT_NULL(status);
    TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetStringValue(status));

    cJSON *duty = cJSON_GetObjectItemCaseSensitive(body, "duty_pct");
    TEST_ASSERT_NOT_NULL(duty);
    TEST_ASSERT_EQUAL_INT(50, (int)cJSON_GetNumberValue(duty));

    TEST_ASSERT_EQUAL_INT(50, g_fan.set_duty_last);

    cJSON_Delete(body);
    bb_fan_set_primary(NULL);
    free(h);
}

// ---------------------------------------------------------------------------
// POST: bad input → 400
// ---------------------------------------------------------------------------

void test_bb_fan_routes_post_no_body_400(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);
    bb_fan_set_primary(h);

    // No body injected → body_len == 0
    bb_http_host_capture_t cap = run_post(NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    bb_fan_set_primary(NULL);
    free(h);
}

void test_bb_fan_routes_post_invalid_json_400(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);
    bb_fan_set_primary(h);

    bb_http_host_capture_t cap = run_post("{not valid json");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    bb_fan_set_primary(NULL);
    free(h);
}

void test_bb_fan_routes_post_missing_duty_pct_400(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);
    bb_fan_set_primary(h);

    bb_http_host_capture_t cap = run_post("{\"other\":50}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    bb_fan_set_primary(NULL);
    free(h);
}

void test_bb_fan_routes_post_out_of_range_400(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);
    bb_fan_set_primary(h);

    bb_http_host_capture_t cap = run_post("{\"duty_pct\":150}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    bb_fan_set_primary(NULL);
    free(h);
}

// ---------------------------------------------------------------------------
// POST: no primary → 503
// ---------------------------------------------------------------------------

void test_bb_fan_routes_post_no_primary_503(void)
{
    bb_fan_set_primary(NULL);

    bb_http_host_capture_t cap = run_post("{\"duty_pct\":50}");
    TEST_ASSERT_EQUAL_INT(503, cap.status);

    cJSON *body = NULL;
    if (cap.body) body = cJSON_Parse(cap.body);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_NOT_NULL_MESSAGE(body, "503 response did not include JSON body");
    cJSON *err = cJSON_GetObjectItemCaseSensitive(body, "error");
    TEST_ASSERT_NOT_NULL(err);
    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// Schema: base fields + extender fragment
// ---------------------------------------------------------------------------

void test_bb_fan_routes_schema_contains_base_fields(void)
{
    const char *schema = bb_fan_routes_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"present\""), "present not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"rpm\""),     "rpm not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"duty_pct\""), "duty_pct not in schema");
}

void test_bb_fan_routes_schema_contains_extender_fragment(void)
{
    static const char frag[] = "\"autofan_target_c\":{\"type\":\"number\"}";
    bb_http_register_route_extender("fan",
                                    (bb_http_extender_fn)fan_extender_add_extra,
                                    frag);
    const char *schema = bb_fan_routes_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag),
        "extender fragment not found in assembled schema");
}

// ---------------------------------------------------------------------------
// Autofan-specific route tests (CONFIG_BB_FAN_AUTOFAN)
// ---------------------------------------------------------------------------

#ifdef CONFIG_BB_FAN_AUTOFAN

// Local autofan GET handler that calls bb_fan_routes_emit (the real emit path).
static cJSON *run_af_get(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    // Use the bb_fan_routes_emit path from the host twin via the test-exported emit fn.
    // bb_fan_routes_emit is not directly exposed on host, so we call the same logic
    // that bb_fan_routes.c (espidf) uses by invoking through bb_fan_get_autofan_telemetry.
    // For the vr-label test we verify the GET handler inline using bb_fan_snapshot
    // and bb_fan_get_autofan_telemetry, mirroring the espidf emit code exactly
    // (including the "aux" → "vr" mapping).
    bb_fan_handle_t h = bb_fan_primary();
    bool present = (h != NULL);
    bb_fan_snapshot_t snap;
    bb_fan_snapshot(h, &snap);
    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_bool(root, "present", present);
    if (present && snap.rpm >= 0) {
        bb_json_obj_set_number(root, "rpm", (double)snap.rpm);
    } else {
        bb_json_obj_set_null(root, "rpm");
    }
    if (present && snap.duty_pct >= 0) {
        bb_json_obj_set_number(root, "duty_pct", (double)snap.duty_pct);
    } else {
        bb_json_obj_set_null(root, "duty_pct");
    }
    if (present && h) {
        bb_fan_autofan_telemetry_t tel;
        bb_fan_get_autofan_telemetry(h, &tel);
        if (tel.die_ema_c >= 0.0f) {
            bb_json_obj_set_number(root, "die_ema_c", (double)tel.die_ema_c);
        } else {
            bb_json_obj_set_null(root, "die_ema_c");
        }
        if (tel.aux_ema_c >= 0.0f) {
            bb_json_obj_set_number(root, "vr_ema_c", (double)tel.aux_ema_c);
        } else {
            bb_json_obj_set_null(root, "vr_ema_c");
        }
        if (tel.pid_input_c >= 0.0f) {
            bb_json_obj_set_number(root, "pid_input_c", (double)tel.pid_input_c);
        } else {
            bb_json_obj_set_null(root, "pid_input_c");
        }
        // Wire-layer mapping: internal "aux" → TM contract name "vr"
        const char *src = tel.pid_input_src ? tel.pid_input_src : "";
        if (src[0] == 'a') src = "vr";
        bb_json_obj_set_string(root, "pid_input_src", src);
    }
    bb_http_route_run_extenders("fan", root);
    char *str = bb_json_serialize(root);
    bb_json_free(root);
    if (str) {
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send_chunk(req, str, -1);
        bb_http_resp_send_chunk(req, NULL, 0);
        free(str);
    } else {
        bb_http_resp_send_chunk(req, NULL, 0);
    }
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    cJSON *parsed = NULL;
    if (cap.body) parsed = cJSON_Parse(cap.body);
    bb_http_host_capture_free(&cap);
    return parsed;
}

// Local autofan POST handler — mirrors the espidf autofan POST path,
// including invoking the persist callback via bb_fan_routes_invoke_persist_cb.
static bb_http_host_capture_t run_af_post(const char *body)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (body) bb_http_host_capture_set_req_body(body, (int)strlen(body));

    bb_fan_handle_t h = bb_fan_primary();
    if (!h) {
        bb_http_resp_set_status(req, 503);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "no primary fan");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        goto done;
    }

    {
        int body_len = bb_http_req_body_len(req);
        if (body_len <= 0 || body_len > 256) {
            bb_http_resp_set_status(req, 400);
            bb_json_t err = bb_json_obj_new();
            bb_json_obj_set_string(err, "error", "missing or oversized body");
            char *str = bb_json_serialize(err);
            bb_json_free(err);
            if (str) {
                bb_http_resp_set_type(req, "application/json");
                bb_http_resp_send_chunk(req, str, -1);
                bb_http_resp_send_chunk(req, NULL, 0);
                free(str);
            } else {
                bb_http_resp_send_chunk(req, NULL, 0);
            }
            goto done;
        }

        char buf[256];
        int n = bb_http_req_recv(req, buf, sizeof(buf) - 1);
        if (n < 0) {
            bb_http_resp_set_status(req, 400);
            bb_http_resp_send_chunk(req, NULL, 0);
            goto done;
        }
        buf[n] = '\0';

        bb_json_t parsed = bb_json_parse(buf, (size_t)n);
        if (!parsed) {
            bb_http_resp_set_status(req, 400);
            bb_json_t err = bb_json_obj_new();
            bb_json_obj_set_string(err, "error", "invalid JSON");
            char *str = bb_json_serialize(err);
            bb_json_free(err);
            if (str) {
                bb_http_resp_set_type(req, "application/json");
                bb_http_resp_send_chunk(req, str, -1);
                bb_http_resp_send_chunk(req, NULL, 0);
                free(str);
            } else {
                bb_http_resp_send_chunk(req, NULL, 0);
            }
            goto done;
        }

        bb_fan_autofan_cfg_t cfg;
        bb_fan_get_autofan_cfg(h, &cfg);

        double d;
        bool b;
        if (bb_json_obj_get_bool(parsed, "autofan", &b))        cfg.enabled      = b;
        if (bb_json_obj_get_number(parsed, "die_target_c", &d)) cfg.die_target_c = (float)d;
        if (bb_json_obj_get_number(parsed, "vr_target_c",  &d)) cfg.aux_target_c = (float)d;
        if (bb_json_obj_get_number(parsed, "manual_pct",   &d)) cfg.manual_pct   = (int)d;
        if (bb_json_obj_get_number(parsed, "min_pct",      &d)) cfg.min_pct      = (int)d;
        bb_json_free(parsed);

        bb_fan_set_autofan(h, &cfg);
        bb_fan_routes_invoke_persist_cb(&cfg);
        bb_http_resp_no_content(req);
    }

done:;
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    return cap;
}

// ---------------------------------------------------------------------------
// A: pid_input_src emits "vr" (not "aux") for the aux source
// ---------------------------------------------------------------------------

static unsigned long s_af_mock_ms;
static unsigned long af_mock_now(void) { return s_af_mock_ms; }

void test_bb_fan_routes_pid_input_src_emits_vr_for_aux(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);

    // Inject mock clock
    s_af_mock_ms = 0;
    bb_fan_pid_set_mock_clock(h, af_mock_now);

    // Enable autofan with aux source winning
    bb_fan_autofan_cfg_t cfg = {
        .enabled      = true,
        .die_target_c = 60.0f,
        .aux_target_c = 75.0f,
        .min_pct      = 25,
        .manual_pct   = 100,
    };
    bb_fan_set_autofan(h, &cfg);
    bb_fan_set_primary(h);

    // die=61 (ratio~0.017), aux=90 (ratio~0.2) → aux wins → pid_input_src internal = "aux"
    g_fan.die_c = 61.0f;
    g_fan.die_fail = false;
    bb_fan_set_aux_temp(h, 90.0f);
    s_af_mock_ms += 5000;
    bb_fan_poll(h);

    cJSON *body = run_af_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "GET did not return JSON");

    cJSON *src = cJSON_GetObjectItemCaseSensitive(body, "pid_input_src");
    TEST_ASSERT_NOT_NULL_MESSAGE(src, "pid_input_src missing from GET response");
    TEST_ASSERT_EQUAL_STRING("vr", cJSON_GetStringValue(src));

    cJSON_Delete(body);
    bb_fan_set_primary(NULL);
    free(h);
}

void test_bb_fan_routes_pid_input_src_emits_die_for_die(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);

    s_af_mock_ms = 0;
    bb_fan_pid_set_mock_clock(h, af_mock_now);

    bb_fan_autofan_cfg_t cfg = {
        .enabled      = true,
        .die_target_c = 60.0f,
        .aux_target_c = 75.0f,
        .min_pct      = 25,
        .manual_pct   = 100,
    };
    bb_fan_set_autofan(h, &cfg);
    bb_fan_set_primary(h);

    // die=75 (ratio=0.25), aux=80 (ratio~0.067) → die wins
    g_fan.die_c = 75.0f;
    g_fan.die_fail = false;
    bb_fan_set_aux_temp(h, 80.0f);
    s_af_mock_ms += 5000;
    bb_fan_poll(h);

    cJSON *body = run_af_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *src = cJSON_GetObjectItemCaseSensitive(body, "pid_input_src");
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_EQUAL_STRING("die", cJSON_GetStringValue(src));

    cJSON_Delete(body);
    bb_fan_set_primary(NULL);
    free(h);
}

// ---------------------------------------------------------------------------
// B: persist callback — fires on POST, NOT on direct bb_fan_set_autofan()
// ---------------------------------------------------------------------------

typedef struct {
    int call_count;
    bb_fan_autofan_cfg_t last_cfg;
    void *last_ctx_check;
} persist_spy_t;

static void persist_spy_fn(void *ctx, const bb_fan_autofan_cfg_t *cfg)
{
    persist_spy_t *spy = (persist_spy_t *)ctx;
    spy->call_count++;
    spy->last_cfg = *cfg;
    spy->last_ctx_check = ctx;
}

void test_bb_fan_routes_persist_cb_fires_on_post(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);
    bb_fan_set_primary(h);

    persist_spy_t spy = {0};
    bb_fan_routes_set_autofan_persist_cb(persist_spy_fn, &spy);

    // POST with autofan config
    bb_http_host_capture_t cap = run_af_post(
        "{\"autofan\":true,\"die_target_c\":65.0,\"vr_target_c\":80.0,"
        "\"manual_pct\":50,\"min_pct\":30}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    // Callback must have fired exactly once
    TEST_ASSERT_EQUAL_INT(1, spy.call_count);
    TEST_ASSERT_TRUE(spy.last_cfg.enabled);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 65.0f, spy.last_cfg.die_target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, spy.last_cfg.aux_target_c);
    TEST_ASSERT_EQUAL_INT(50, spy.last_cfg.manual_pct);
    TEST_ASSERT_EQUAL_INT(30, spy.last_cfg.min_pct);

    bb_fan_routes_set_autofan_persist_cb(NULL, NULL);
    bb_fan_set_primary(NULL);
    free(h);
}

void test_bb_fan_routes_persist_cb_not_called_by_direct_set(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);

    persist_spy_t spy = {0};
    bb_fan_routes_set_autofan_persist_cb(persist_spy_fn, &spy);

    // Direct call to bb_fan_set_autofan — must NOT invoke persist cb
    bb_fan_autofan_cfg_t cfg = {
        .enabled      = true,
        .die_target_c = 70.0f,
        .aux_target_c = 80.0f,
        .min_pct      = 25,
        .manual_pct   = 60,
    };
    bb_fan_set_autofan(h, &cfg);

    TEST_ASSERT_EQUAL_INT(0, spy.call_count);

    bb_fan_routes_set_autofan_persist_cb(NULL, NULL);
    free(h);
}

void test_bb_fan_routes_persist_cb_not_called_when_null(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_fake, &g_fan, &h);
    bb_fan_set_primary(h);

    // No cb registered — POST must not crash
    bb_fan_routes_set_autofan_persist_cb(NULL, NULL);

    bb_http_host_capture_t cap = run_af_post("{\"autofan\":false,\"manual_pct\":40}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    bb_fan_set_primary(NULL);
    free(h);
}

#endif /* CONFIG_BB_FAN_AUTOFAN */
