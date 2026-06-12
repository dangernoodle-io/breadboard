// Tests for /api/mqtt routes:
// - GET masks password + reports set-flags + connected state
// - PATCH persists each field to NVS "bb_mqtt"
// - PATCH bad body → 400
#include "unity.h"
#include "bb_mqtt.h"
#include "bb_mqtt_routes.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// Test hook declarations from host twin.
bb_err_t bb_mqtt_routes_get_handler_for_test(bb_http_request_t *req);
bb_err_t bb_mqtt_routes_patch_handler_for_test(bb_http_request_t *req);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static cJSON *run_get(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_mqtt_routes_get_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    cJSON *parsed = NULL;
    if (cap.body) parsed = cJSON_Parse(cap.body);
    bb_http_host_capture_free(&cap);
    return parsed;
}

static bb_http_host_capture_t run_patch(const char *body)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (body) bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_mqtt_routes_patch_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    return cap;
}

// ---------------------------------------------------------------------------
// GET: default state (all empty NVS)
// ---------------------------------------------------------------------------

void test_bb_mqtt_routes_get_empty_nvs(void)
{
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "GET did not emit valid JSON");

    cJSON *uri = cJSON_GetObjectItemCaseSensitive(body, "uri");
    TEST_ASSERT_NOT_NULL(uri);
    TEST_ASSERT_TRUE(cJSON_IsString(uri));

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_FALSE(cJSON_IsTrue(connected));  // no client set → false

    cJSON *tls = cJSON_GetObjectItemCaseSensitive(body, "tls");
    TEST_ASSERT_NOT_NULL(tls);
    TEST_ASSERT_FALSE(cJSON_IsTrue(tls));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET: password is masked ("***"), never returned as plaintext
// ---------------------------------------------------------------------------

void test_bb_mqtt_routes_get_masks_password(void)
{
    bb_nv_set_str("bb_mqtt", "password", "super-secret");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *pw = cJSON_GetObjectItemCaseSensitive(body, "password");
    TEST_ASSERT_NOT_NULL(pw);
    TEST_ASSERT_TRUE(cJSON_IsString(pw));
    // Must not reveal actual password.
    TEST_ASSERT_NULL(strstr(cJSON_GetStringValue(pw), "super-secret"));
    // Must be the mask sentinel.
    TEST_ASSERT_EQUAL_STRING("***", cJSON_GetStringValue(pw));

    cJSON_Delete(body);
}

void test_bb_mqtt_routes_get_password_empty_when_not_set(void)
{
    // password not in NVS → empty string (not "***")
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *pw = cJSON_GetObjectItemCaseSensitive(body, "password");
    TEST_ASSERT_NOT_NULL(pw);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetStringValue(pw));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET: ca_set / cert_set / key_set reflect NVS presence
// ---------------------------------------------------------------------------

void test_bb_mqtt_routes_get_ca_set_true_when_nvs_has_ca(void)
{
    bb_nv_set_str("bb_mqtt", "tls_ca", "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_TRUE(cJSON_IsTrue(ca_set));

    cJSON_Delete(body);
}

void test_bb_mqtt_routes_get_ca_set_false_when_not_in_nvs(void)
{
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_FALSE(cJSON_IsTrue(ca_set));

    cJSON_Delete(body);
}

void test_bb_mqtt_routes_get_cert_set_and_key_set_flags(void)
{
    bb_nv_set_str("bb_mqtt", "tls_cert", "-----BEGIN CERTIFICATE-----\nfake_cert\n-----END CERTIFICATE-----\n");
    bb_nv_set_str("bb_mqtt", "tls_key",  "-----BEGIN PRIVATE KEY-----\nfake_key\n-----END PRIVATE KEY-----\n");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *cert_set = cJSON_GetObjectItemCaseSensitive(body, "cert_set");
    TEST_ASSERT_NOT_NULL(cert_set);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cert_set));

    cJSON *key_set = cJSON_GetObjectItemCaseSensitive(body, "key_set");
    TEST_ASSERT_NOT_NULL(key_set);
    TEST_ASSERT_TRUE(cJSON_IsTrue(key_set));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET: connected reflects client handle state
// ---------------------------------------------------------------------------

void test_bb_mqtt_routes_get_connected_from_client(void)
{
    bb_mqtt_t client = NULL;
    bb_mqtt_cfg_t cfg = {
        .uri = "mqtt://localhost:1883",
    };
    bb_mqtt_init(&cfg, &client);
    bb_mqtt_host_set_connected(client, true);
    bb_mqtt_routes_set_client(&client);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_TRUE(cJSON_IsTrue(connected));

    cJSON_Delete(body);
    bb_mqtt_routes_set_client(NULL);
    bb_mqtt_destroy(client);
}

void test_bb_mqtt_routes_get_connected_false_when_disconnected(void)
{
    bb_mqtt_t client = NULL;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_init(&cfg, &client);
    bb_mqtt_host_set_connected(client, false);
    bb_mqtt_routes_set_client(&client);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_FALSE(cJSON_IsTrue(connected));

    cJSON_Delete(body);
    bb_mqtt_routes_set_client(NULL);
    bb_mqtt_destroy(client);
}

// ---------------------------------------------------------------------------
// GET: connected falls back to bb_mqtt_default() when no set_client called
// ---------------------------------------------------------------------------

void test_bb_mqtt_routes_get_connected_via_default(void)
{
    // No bb_mqtt_routes_set_client call — route should fall back to
    // bb_mqtt_default().
    bb_mqtt_t client = NULL;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_init(&cfg, &client);
    bb_mqtt_host_set_connected(client, true);
    bb_mqtt_default_set(client);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_TRUE(cJSON_IsTrue(connected));

    cJSON_Delete(body);
    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(client);
}

void test_bb_mqtt_routes_get_connected_false_via_default_when_disconnected(void)
{
    bb_mqtt_t client = NULL;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_init(&cfg, &client);
    bb_mqtt_host_set_connected(client, false);
    bb_mqtt_default_set(client);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_FALSE(cJSON_IsTrue(connected));

    cJSON_Delete(body);
    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(client);
}

// ---------------------------------------------------------------------------
// PATCH: persists each field to NVS
// ---------------------------------------------------------------------------

void test_bb_mqtt_routes_patch_persists_uri(void)
{
    bb_http_host_capture_t cap = run_patch("{\"uri\":\"mqtt://broker.example.com:1883\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[128] = {0};
    bb_nv_get_str("bb_mqtt", "uri", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.example.com:1883", buf);
}

void test_bb_mqtt_routes_patch_persists_client_id(void)
{
    bb_http_host_capture_t cap = run_patch("{\"client_id\":\"my-sensor-01\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[64] = {0};
    bb_nv_get_str("bb_mqtt", "client_id", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("my-sensor-01", buf);
}

void test_bb_mqtt_routes_patch_persists_username(void)
{
    bb_http_host_capture_t cap = run_patch("{\"username\":\"acme-user\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[64] = {0};
    bb_nv_get_str("bb_mqtt", "username", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("acme-user", buf);
}

void test_bb_mqtt_routes_patch_persists_password(void)
{
    bb_http_host_capture_t cap = run_patch("{\"password\":\"tk-test-000\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[64] = {0};
    bb_nv_get_str("bb_mqtt", "password", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("tk-test-000", buf);
}

void test_bb_mqtt_routes_patch_persists_tls_ca(void)
{
    bb_http_host_capture_t cap = run_patch("{\"tls_ca\":\"-----BEGIN CERTIFICATE-----\\ntest\\n-----END CERTIFICATE-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[256] = {0};
    bb_nv_get_str("bb_mqtt", "tls_ca", buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_mqtt_routes_patch_persists_enabled(void)
{
    bb_http_host_capture_t cap = run_patch("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "enabled", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_bb_mqtt_routes_patch_persists_tls_flag(void)
{
    bb_http_host_capture_t cap = run_patch("{\"tls\":true}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "tls", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_bb_mqtt_routes_patch_partial_update_leaves_others(void)
{
    // Pre-populate uri, then PATCH only client_id.
    bb_nv_set_str("bb_mqtt", "uri", "mqtt://original.example.com:1883");
    bb_http_host_capture_t cap = run_patch("{\"client_id\":\"new-id\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    // uri must be unchanged.
    char buf[128] = {0};
    bb_nv_get_str("bb_mqtt", "uri", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("mqtt://original.example.com:1883", buf);

    char id_buf[64] = {0};
    bb_nv_get_str("bb_mqtt", "client_id", id_buf, sizeof(id_buf), "");
    TEST_ASSERT_EQUAL_STRING("new-id", id_buf);
}

// ---------------------------------------------------------------------------
// PATCH: bad body → 400
// ---------------------------------------------------------------------------

void test_bb_mqtt_routes_patch_no_body_400(void)
{
    bb_http_host_capture_t cap = run_patch(NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_bb_mqtt_routes_patch_invalid_json_400(void)
{
    bb_http_host_capture_t cap = run_patch("not-json-at-all");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}
