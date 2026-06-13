// Tests for bb_sink_http_telemetry section:
// - GET masks TLS creds (ca_set/cert_set/key_set) + default path_tmpl
// - PATCH persists each field (base, path_tmpl, tls_ca/cert/key, qos, enabled)
#include "unity.h"
#include "bb_sink_http_telemetry.h"
#include "bb_nv.h"
#include "bb_json.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// Test hook declarations from host twin.
void     bb_sink_http_telemetry_section_get_for_test(bb_json_t section, void *ctx);
bb_err_t bb_sink_http_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static cJSON *run_get(void)
{
    bb_json_t section = bb_json_obj_new();
    bb_sink_http_telemetry_section_get_for_test(section, NULL);
    char *s = bb_json_serialize(section);
    bb_json_free(section);
    if (!s) return NULL;
    cJSON *parsed = cJSON_Parse(s);
    bb_json_free_str(s);
    return parsed;
}

static bb_err_t run_patch(const char *body_json)
{
    bb_json_t patch = bb_json_parse(body_json, 0);
    if (!patch) return BB_ERR_INVALID_ARG;
    bb_err_t rc = bb_sink_http_telemetry_section_patch_for_test(patch, NULL);
    bb_json_free(patch);
    return rc;
}

// ---------------------------------------------------------------------------
// GET: default state
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_get_empty_nvs(void)
{
    bb_sink_http_telemetry_reset_for_test();
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

void test_bb_sink_http_telemetry_get_default_path_tmpl(void)
{
    bb_sink_http_telemetry_reset_for_test();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *pt = cJSON_GetObjectItemCaseSensitive(body, "path_tmpl");
    TEST_ASSERT_NOT_NULL(pt);
    TEST_ASSERT_TRUE(cJSON_IsString(pt));
    TEST_ASSERT_EQUAL_STRING("/topics/{topic}?qos={qos}", cJSON_GetStringValue(pt));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET: ca_set / cert_set / key_set reflect NVS presence
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_get_ca_set_true_when_nvs_has_ca(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_nv_set_str("bb_sink_http", "tls_ca",
                  "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_TRUE(cJSON_IsTrue(ca_set));

    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(body, "tls_ca"));

    cJSON_Delete(body);
}

void test_bb_sink_http_telemetry_get_ca_set_false_when_not_in_nvs(void)
{
    bb_sink_http_telemetry_reset_for_test();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_FALSE(cJSON_IsTrue(ca_set));

    cJSON_Delete(body);
}

void test_bb_sink_http_telemetry_get_cert_set_and_key_set_flags(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_nv_set_str("bb_sink_http", "tls_cert",
                  "-----BEGIN CERTIFICATE-----\nfake_cert\n-----END CERTIFICATE-----\n");
    bb_nv_set_str("bb_sink_http", "tls_key",
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
// PATCH: persists each field to NVS "bb_sink_http"
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_patch_persists_base(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"base\":\"https://xxxx-ats.iot.us-east-1.amazonaws.com:8443\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[128] = {0};
    bb_nv_get_str("bb_sink_http", "base", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("https://xxxx-ats.iot.us-east-1.amazonaws.com:8443", buf);
}

void test_bb_sink_http_telemetry_patch_persists_path_tmpl(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"path_tmpl\":\"/v1/{topic}?qos={qos}\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[128] = {0};
    bb_nv_get_str("bb_sink_http", "path_tmpl", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("/v1/{topic}?qos={qos}", buf);
}

void test_bb_sink_http_telemetry_patch_persists_tls_ca(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"tls_ca\":\"-----BEGIN CERTIFICATE-----\\ntest\\n-----END CERTIFICATE-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[256] = {0};
    bb_nv_get_str("bb_sink_http", "tls_ca", buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_sink_http_telemetry_patch_persists_tls_cert(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"tls_cert\":\"-----BEGIN CERTIFICATE-----\\ncert\\n-----END CERTIFICATE-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[256] = {0};
    bb_nv_get_str("bb_sink_http", "tls_cert", buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_sink_http_telemetry_patch_persists_tls_key(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"tls_key\":\"-----BEGIN PRIVATE KEY-----\\nkey\\n-----END PRIVATE KEY-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[256] = {0};
    bb_nv_get_str("bb_sink_http", "tls_key", buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_sink_http_telemetry_patch_persists_qos(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"qos\":2}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[4] = {0};
    bb_nv_get_str("bb_sink_http", "qos", buf, sizeof(buf), "1");
    TEST_ASSERT_EQUAL_STRING("2", buf);
}

void test_bb_sink_http_telemetry_patch_persists_enabled(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[4] = {0};
    bb_nv_get_str("bb_sink_http", "enabled", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_bb_sink_http_telemetry_patch_partial_update_leaves_others(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_nv_set_str("bb_sink_http", "base", "https://original.example.com:8443");
    bb_err_t rc = run_patch("{\"qos\":0}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[128] = {0};
    bb_nv_get_str("bb_sink_http", "base", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("https://original.example.com:8443", buf);
}
