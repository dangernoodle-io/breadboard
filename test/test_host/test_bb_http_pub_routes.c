// Tests for /api/httppub routes:
// - GET masks TLS creds (reports ca_set/cert_set/key_set, never raw PEM)
// - GET reports default path_tmpl when NVS empty
// - PATCH persists each field (base, path_tmpl, tls_ca/cert/key, qos, enabled)
// - PATCH bad body → 400
#include "unity.h"
#include "bb_http_pub_routes.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// Test hook declarations from host twin.
bb_err_t bb_http_pub_routes_get_handler_for_test(bb_http_request_t *req);
bb_err_t bb_http_pub_routes_patch_handler_for_test(bb_http_request_t *req);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static cJSON *run_get(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_pub_routes_get_handler_for_test(req);
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
    bb_http_pub_routes_patch_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    return cap;
}

// ---------------------------------------------------------------------------
// GET: default state
// ---------------------------------------------------------------------------

void test_bb_http_pub_routes_get_empty_nvs(void)
{
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "GET did not emit valid JSON");

    cJSON *base = cJSON_GetObjectItemCaseSensitive(body, "base");
    TEST_ASSERT_NOT_NULL(base);
    TEST_ASSERT_TRUE(cJSON_IsString(base));

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    TEST_ASSERT_NOT_NULL(enabled);
    TEST_ASSERT_FALSE(cJSON_IsTrue(enabled));

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_FALSE(cJSON_IsTrue(ca_set));

    cJSON_Delete(body);
}

void test_bb_http_pub_routes_get_default_path_tmpl(void)
{
    // When NVS has no path_tmpl, GET must return the default.
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *pt = cJSON_GetObjectItemCaseSensitive(body, "path_tmpl");
    TEST_ASSERT_NOT_NULL(pt);
    TEST_ASSERT_TRUE(cJSON_IsString(pt));
    TEST_ASSERT_EQUAL_STRING("/topics/{topic}?qos={qos}", cJSON_GetStringValue(pt));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET: ca_set / cert_set / key_set reflect NVS presence; raw PEM not returned
// ---------------------------------------------------------------------------

void test_bb_http_pub_routes_get_ca_set_true_when_nvs_has_ca(void)
{
    bb_nv_set_str("bb_http_pub", "tls_ca",
                  "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_TRUE(cJSON_IsTrue(ca_set));

    // Raw PEM must not appear in the response.
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(body, "tls_ca"));

    cJSON_Delete(body);
}

void test_bb_http_pub_routes_get_ca_set_false_when_not_in_nvs(void)
{
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_FALSE(cJSON_IsTrue(ca_set));

    cJSON_Delete(body);
}

void test_bb_http_pub_routes_get_cert_set_and_key_set_flags(void)
{
    bb_nv_set_str("bb_http_pub", "tls_cert",
                  "-----BEGIN CERTIFICATE-----\nfake_cert\n-----END CERTIFICATE-----\n");
    bb_nv_set_str("bb_http_pub", "tls_key",
                  "-----BEGIN PRIVATE KEY-----\nfake_key\n-----END PRIVATE KEY-----\n");

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
// PATCH: persists each field to NVS "bb_http_pub"
// ---------------------------------------------------------------------------

void test_bb_http_pub_routes_patch_persists_base(void)
{
    bb_http_host_capture_t cap = run_patch(
        "{\"base\":\"https://xxxx-ats.iot.us-east-1.amazonaws.com:8443\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[128] = {0};
    bb_nv_get_str("bb_http_pub", "base", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("https://xxxx-ats.iot.us-east-1.amazonaws.com:8443", buf);
}

void test_bb_http_pub_routes_patch_persists_path_tmpl(void)
{
    bb_http_host_capture_t cap = run_patch("{\"path_tmpl\":\"/v1/{topic}?qos={qos}\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[128] = {0};
    bb_nv_get_str("bb_http_pub", "path_tmpl", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("/v1/{topic}?qos={qos}", buf);
}

void test_bb_http_pub_routes_patch_persists_tls_ca(void)
{
    bb_http_host_capture_t cap = run_patch(
        "{\"tls_ca\":\"-----BEGIN CERTIFICATE-----\\ntest\\n-----END CERTIFICATE-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[256] = {0};
    bb_nv_get_str("bb_http_pub", "tls_ca", buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_http_pub_routes_patch_persists_tls_cert(void)
{
    bb_http_host_capture_t cap = run_patch(
        "{\"tls_cert\":\"-----BEGIN CERTIFICATE-----\\ncert\\n-----END CERTIFICATE-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[256] = {0};
    bb_nv_get_str("bb_http_pub", "tls_cert", buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_http_pub_routes_patch_persists_tls_key(void)
{
    bb_http_host_capture_t cap = run_patch(
        "{\"tls_key\":\"-----BEGIN PRIVATE KEY-----\\nkey\\n-----END PRIVATE KEY-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[256] = {0};
    bb_nv_get_str("bb_http_pub", "tls_key", buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_http_pub_routes_patch_persists_qos(void)
{
    bb_http_host_capture_t cap = run_patch("{\"qos\":2}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[4] = {0};
    bb_nv_get_str("bb_http_pub", "qos", buf, sizeof(buf), "1");
    TEST_ASSERT_EQUAL_STRING("2", buf);
}

void test_bb_http_pub_routes_patch_persists_enabled(void)
{
    bb_http_host_capture_t cap = run_patch("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    char buf[4] = {0};
    bb_nv_get_str("bb_http_pub", "enabled", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_bb_http_pub_routes_patch_partial_update_leaves_others(void)
{
    bb_nv_set_str("bb_http_pub", "base", "https://original.example.com:8443");

    bb_http_host_capture_t cap = run_patch("{\"qos\":0}");
    TEST_ASSERT_EQUAL_INT(204, cap.status);
    bb_http_host_capture_free(&cap);

    // base must be unchanged
    char buf[128] = {0};
    bb_nv_get_str("bb_http_pub", "base", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("https://original.example.com:8443", buf);
}

// ---------------------------------------------------------------------------
// PATCH: bad body → 400
// ---------------------------------------------------------------------------

void test_bb_http_pub_routes_patch_no_body_returns_400(void)
{
    bb_http_host_capture_t cap = run_patch(NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_bb_http_pub_routes_patch_invalid_json_returns_400(void)
{
    bb_http_host_capture_t cap = run_patch("not-json-at-all");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}
