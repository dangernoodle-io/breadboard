// Tests for bb_nv_erase_namespace and the NVS delete route handler (B1-290).
//
// Covers:
//   1. bb_nv_erase_namespace: erase then bb_nv_exists is false.
//   2. bb_nv_delete_handler (body-based DELETE /api/nvs):
//       - namespace string, no key    → clears namespace; 200 {"deleted":["ns"]}
//       - namespace string + key      → clears single key; 200 {"deleted":["ns"],"key":"k"}
//       - namespace array             → clears each; 200 {"deleted":[...]}
//       - missing confirm             → 412
//       - confirm:false               → 412
//       - bb_cfg without wipe_wifi    → 412
//       - key + array namespace       → 400 (ambiguous)
//       - missing namespace field     → 400
//       - no body                     → 400

#include "unity.h"
#include "bb_nv.h"
#include "bb_nv_keys.h"
#include "bb_nv_delete_routes.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Helper: run handler with a JSON body string
// ---------------------------------------------------------------------------

static bb_http_host_capture_t run_handler_body(const char *json_body)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (json_body) {
        bb_http_host_capture_set_req_body(json_body, (int)strlen(json_body));
    }
    bb_nv_delete_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    return cap;
}

// ---------------------------------------------------------------------------
// bb_nv_erase_namespace unit tests (kept: portable, not route-specific)
// ---------------------------------------------------------------------------

void test_nv_erase_namespace_null_returns_err(void)
{
    bb_err_t err = bb_nv_erase_namespace(NULL);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_nv_erase_namespace_removes_all_keys(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("test_ns", "key_a", "val_a");
    bb_nv_set_str("test_ns", "key_b", "val_b");
    TEST_ASSERT_TRUE(bb_nv_exists("test_ns", "key_a"));
    TEST_ASSERT_TRUE(bb_nv_exists("test_ns", "key_b"));

    bb_err_t err = bb_nv_erase_namespace("test_ns");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    TEST_ASSERT_FALSE(bb_nv_exists("test_ns", "key_a"));
    TEST_ASSERT_FALSE(bb_nv_exists("test_ns", "key_b"));
}

void test_nv_erase_namespace_does_not_affect_other_namespaces(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("ns_a", "key", "v");
    bb_nv_set_str("ns_b", "key", "v");

    bb_nv_erase_namespace("ns_a");

    TEST_ASSERT_FALSE(bb_nv_exists("ns_a", "key"));
    TEST_ASSERT_TRUE(bb_nv_exists("ns_b", "key"));
}

void test_nv_erase_namespace_empty_is_ok(void)
{
    bb_nv_host_str_store_reset();
    bb_err_t err = bb_nv_erase_namespace("nonexistent_ns");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

void test_nv_erase_namespace_then_set_works(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("my_ns", "k", "old");
    bb_nv_erase_namespace("my_ns");
    bb_err_t err = bb_nv_set_str("my_ns", "k", "new");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_TRUE(bb_nv_exists("my_ns", "k"));
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — missing / no body
// ---------------------------------------------------------------------------

void test_nvs_delete_no_body_returns_400(void)
{
    bb_nv_host_str_store_reset();
    bb_http_host_capture_t cap = run_handler_body(NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — confirm guards
// ---------------------------------------------------------------------------

void test_nvs_delete_missing_confirm_returns_412(void)
{
    bb_nv_host_str_store_reset();
    /* confirm absent */
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"test_ns\"}");
    TEST_ASSERT_EQUAL_INT(412, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_nvs_delete_confirm_false_returns_412(void)
{
    bb_nv_host_str_store_reset();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"test_ns\",\"confirm\":false}");
    TEST_ASSERT_EQUAL_INT(412, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — missing namespace
// ---------------------------------------------------------------------------

void test_nvs_delete_missing_namespace_returns_400(void)
{
    bb_nv_host_str_store_reset();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — namespace string, clear whole namespace
// ---------------------------------------------------------------------------

void test_nvs_delete_ns_string_clears_namespace_returns_200(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("bb_mqtt", "broker", "mqtt://example.com");
    TEST_ASSERT_TRUE(bb_nv_exists("bb_mqtt", "broker"));

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_mqtt\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);

    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *del = cJSON_GetObjectItemCaseSensitive(j, "deleted");
    TEST_ASSERT_NOT_NULL(del);
    TEST_ASSERT_TRUE(cJSON_IsArray(del));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(del));
    TEST_ASSERT_EQUAL_STRING("bb_mqtt", cJSON_GetArrayItem(del, 0)->valuestring);
    cJSON_Delete(j);

    TEST_ASSERT_FALSE(bb_nv_exists("bb_mqtt", "broker"));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — namespace string + key, clear single key
// ---------------------------------------------------------------------------

void test_nvs_delete_ns_string_with_key_clears_key_returns_200(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("bb_mqtt", "broker", "mqtt://example.com");
    bb_nv_set_str("bb_mqtt", "qos",    "1");
    TEST_ASSERT_TRUE(bb_nv_exists("bb_mqtt", "broker"));
    TEST_ASSERT_TRUE(bb_nv_exists("bb_mqtt", "qos"));

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_mqtt\",\"key\":\"broker\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);

    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *del = cJSON_GetObjectItemCaseSensitive(j, "deleted");
    TEST_ASSERT_NOT_NULL(del);
    TEST_ASSERT_TRUE(cJSON_IsArray(del));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(del));
    TEST_ASSERT_EQUAL_STRING("bb_mqtt", cJSON_GetArrayItem(del, 0)->valuestring);
    cJSON *key_field = cJSON_GetObjectItemCaseSensitive(j, "key");
    TEST_ASSERT_NOT_NULL(key_field);
    TEST_ASSERT_EQUAL_STRING("broker", key_field->valuestring);
    cJSON_Delete(j);

    /* Only "broker" erased; "qos" still exists */
    TEST_ASSERT_FALSE(bb_nv_exists("bb_mqtt", "broker"));
    TEST_ASSERT_TRUE(bb_nv_exists("bb_mqtt", "qos"));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — namespace array clears each namespace
// ---------------------------------------------------------------------------

void test_nvs_delete_ns_array_clears_each_returns_200(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("bb_mqtt",      "broker",  "mqtt://example.com");
    bb_nv_set_str("bb_sink_http", "base",    "https://example.com");
    bb_nv_set_str(BB_PUB_NVS_NS,  BB_PUB_NVS_KEY_ENABLED, "1");
    TEST_ASSERT_TRUE(bb_nv_exists("bb_mqtt",      "broker"));
    TEST_ASSERT_TRUE(bb_nv_exists("bb_sink_http", "base"));
    TEST_ASSERT_TRUE(bb_nv_exists(BB_PUB_NVS_NS,   BB_PUB_NVS_KEY_ENABLED));

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\",\"bb_sink_http\",\"bb_pub\"],\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);

    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *del = cJSON_GetObjectItemCaseSensitive(j, "deleted");
    TEST_ASSERT_NOT_NULL(del);
    TEST_ASSERT_TRUE(cJSON_IsArray(del));
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(del));
    cJSON_Delete(j);

    TEST_ASSERT_FALSE(bb_nv_exists("bb_mqtt",      "broker"));
    TEST_ASSERT_FALSE(bb_nv_exists("bb_sink_http", "base"));
    TEST_ASSERT_FALSE(bb_nv_exists(BB_PUB_NVS_NS,   BB_PUB_NVS_KEY_ENABLED));
    bb_http_host_capture_free(&cap);
}

void test_nvs_delete_ns_array_leaves_other_ns_intact(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("bb_cfg",  "hostname", "test-host");
    bb_nv_set_str("bb_mqtt", "broker",   "mqtt://example.com");

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\"],\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);

    TEST_ASSERT_TRUE(bb_nv_exists("bb_cfg", "hostname"));
    TEST_ASSERT_FALSE(bb_nv_exists("bb_mqtt", "broker"));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — bb_cfg without wipe_wifi → 412
// ---------------------------------------------------------------------------

void test_nvs_delete_bb_cfg_without_wipe_wifi_returns_412(void)
{
    bb_nv_host_str_store_reset();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_cfg\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(412, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_nvs_delete_bb_cfg_with_wipe_wifi_returns_200(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("bb_cfg", "wifi_ssid", "MyNet");
    TEST_ASSERT_TRUE(bb_nv_exists("bb_cfg", "wifi_ssid"));

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_cfg\",\"confirm\":true,\"wipe_wifi\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_FALSE(bb_nv_exists("bb_cfg", "wifi_ssid"));
    bb_http_host_capture_free(&cap);
}

void test_nvs_delete_array_with_bb_cfg_no_wipe_wifi_returns_412(void)
{
    bb_nv_host_str_store_reset();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\",\"bb_cfg\"],\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(412, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — key + array namespace → 400 (ambiguous)
// ---------------------------------------------------------------------------

void test_nvs_delete_key_with_array_ns_returns_400(void)
{
    bb_nv_host_str_store_reset();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\",\"bb_pub\"],\"key\":\"broker\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}
