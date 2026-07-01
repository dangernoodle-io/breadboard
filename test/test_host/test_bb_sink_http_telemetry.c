// Tests for bb_sink_http_telemetry section:
// - GET masks TLS creds (ca_set/cert_set/key_set) + default path_tmpl
// - PATCH persists each field (base, path_tmpl, tls_ca/cert/key, qos, enabled)
// - GET emits client_id (clear) + headers array with per-row secret masking
// - PATCH persists client_id; PATCH headers array with secret-preserve merge
#include "unity.h"
#include "bb_sink_http_telemetry.h"
#include "bb_sink_http.h"
#include "bb_nv.h"
#include "bb_nv_keys.h"
#include "bb_json.h"
#include "cJSON.h"

#include <stdio.h>
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
// GET: tls boolean derived from base URL scheme
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_get_tls_false_for_http_base(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_nv_set_str("bb_sink_http", "base", "http://broker.example.com:1883");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *tls = cJSON_GetObjectItemCaseSensitive(body, "tls");
    TEST_ASSERT_NOT_NULL(tls);
    TEST_ASSERT_FALSE(cJSON_IsTrue(tls));

    cJSON_Delete(body);
}

void test_bb_sink_http_telemetry_get_tls_true_for_https_base(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_nv_set_str("bb_sink_http", "base", "https://xxxx-ats.iot.us-east-1.amazonaws.com:8443");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *tls = cJSON_GetObjectItemCaseSensitive(body, "tls");
    TEST_ASSERT_NOT_NULL(tls);
    TEST_ASSERT_TRUE(cJSON_IsTrue(tls));

    cJSON_Delete(body);
}

void test_bb_sink_http_telemetry_get_tls_false_when_no_base(void)
{
    bb_sink_http_telemetry_reset_for_test();

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *tls = cJSON_GetObjectItemCaseSensitive(body, "tls");
    TEST_ASSERT_NOT_NULL(tls);
    TEST_ASSERT_FALSE(cJSON_IsTrue(tls));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET: ca_set / cert_set / key_set reflect NVS presence
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_get_ca_set_true_when_nvs_has_ca(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_TLS_CA,
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
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_TLS_CERT,
                  "-----BEGIN CERTIFICATE-----\nfake_cert\n-----END CERTIFICATE-----\n");
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_TLS_KEY,
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
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_PATH_TMPL, buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("/v1/{topic}?qos={qos}", buf);
}

void test_bb_sink_http_telemetry_patch_persists_tls_ca(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"tls_ca\":\"-----BEGIN CERTIFICATE-----\\ntest\\n-----END CERTIFICATE-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[256] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_TLS_CA, buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_sink_http_telemetry_patch_persists_tls_cert(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"tls_cert\":\"-----BEGIN CERTIFICATE-----\\ncert\\n-----END CERTIFICATE-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[256] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_TLS_CERT, buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_sink_http_telemetry_patch_persists_tls_key(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"tls_key\":\"-----BEGIN PRIVATE KEY-----\\nkey\\n-----END PRIVATE KEY-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[256] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_TLS_KEY, buf, sizeof(buf), "");
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

// ---------------------------------------------------------------------------
// GET: client_id field
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_get_client_id_empty_by_default(void)
{
    bb_sink_http_telemetry_reset_for_test();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *cid = cJSON_GetObjectItemCaseSensitive(body, "client_id");
    TEST_ASSERT_NOT_NULL(cid);
    TEST_ASSERT_TRUE(cJSON_IsString(cid));
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetStringValue(cid));

    cJSON_Delete(body);
}

void test_bb_sink_http_telemetry_get_client_id_reflects_nvs(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_CLIENT_ID, "acme-device-01");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *cid = cJSON_GetObjectItemCaseSensitive(body, "client_id");
    TEST_ASSERT_NOT_NULL(cid);
    TEST_ASSERT_EQUAL_STRING("acme-device-01", cJSON_GetStringValue(cid));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// PATCH: client_id persisted to NVS
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_patch_persists_client_id(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"client_id\":\"my-device-42\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[64] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_CLIENT_ID, buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("my-device-42", buf);
}

// ---------------------------------------------------------------------------
// GET: headers array — secret masking
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_get_headers_empty_by_default(void)
{
    bb_sink_http_telemetry_reset_for_test();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *headers = cJSON_GetObjectItemCaseSensitive(body, "headers");
    TEST_ASSERT_NOT_NULL(headers);
    TEST_ASSERT_TRUE(cJSON_IsArray(headers));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(headers));

    cJSON_Delete(body);
}

void test_bb_sink_http_telemetry_get_headers_non_secret_includes_value(void)
{
    bb_sink_http_telemetry_reset_for_test();
    // Store a non-secret header via NVS directly (no '*' prefix).
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, "X-Trace: abc123");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *headers = cJSON_GetObjectItemCaseSensitive(body, "headers");
    TEST_ASSERT_NOT_NULL(headers);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(headers));

    cJSON *entry = cJSON_GetArrayItem(headers, 0);
    TEST_ASSERT_NOT_NULL(entry);

    cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
    TEST_ASSERT_EQUAL_STRING("X-Trace", cJSON_GetStringValue(name));

    cJSON *secret = cJSON_GetObjectItemCaseSensitive(entry, "secret");
    TEST_ASSERT_FALSE(cJSON_IsTrue(secret));

    cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "value");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_STRING("abc123", cJSON_GetStringValue(value));

    cJSON_Delete(body);
}

void test_bb_sink_http_telemetry_get_headers_secret_omits_value_has_set(void)
{
    bb_sink_http_telemetry_reset_for_test();
    // Store a secret header via NVS ('*' prefix).
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, "*Authorization: Bearer xyz");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *headers = cJSON_GetObjectItemCaseSensitive(body, "headers");
    TEST_ASSERT_NOT_NULL(headers);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(headers));

    cJSON *entry = cJSON_GetArrayItem(headers, 0);
    TEST_ASSERT_NOT_NULL(entry);

    // name and secret must be present.
    cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
    TEST_ASSERT_EQUAL_STRING("Authorization", cJSON_GetStringValue(name));

    cJSON *secret = cJSON_GetObjectItemCaseSensitive(entry, "secret");
    TEST_ASSERT_TRUE(cJSON_IsTrue(secret));

    // value must be ABSENT.
    cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "value");
    TEST_ASSERT_NULL(value);

    // set must be true.
    cJSON *set = cJSON_GetObjectItemCaseSensitive(entry, "set");
    TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_TRUE(cJSON_IsTrue(set));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// PATCH: headers array — add non-secret, add secret, secret+blank preserves
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_patch_headers_add_non_secret(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"headers\":[{\"name\":\"X-Trace\",\"value\":\"abc\",\"secret\":false}]}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[512] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, buf, sizeof(buf), "");
    TEST_ASSERT_NOT_NULL(strstr(buf, "X-Trace: abc"));
    // No '*' prefix.
    TEST_ASSERT_NULL(strstr(buf, "*X-Trace"));
}

void test_bb_sink_http_telemetry_patch_headers_add_secret_stores_value(void)
{
    bb_sink_http_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"headers\":[{\"name\":\"Authorization\",\"value\":\"Bearer tok\",\"secret\":true}]}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[512] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, buf, sizeof(buf), "");
    // Must be stored with '*' prefix.
    TEST_ASSERT_NOT_NULL(strstr(buf, "*Authorization: Bearer tok"));
}

void test_bb_sink_http_telemetry_patch_headers_secret_blank_preserves_existing(void)
{
    bb_sink_http_telemetry_reset_for_test();
    // Pre-populate NVS with a stored secret value.
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, "*Authorization: Bearer original");

    // PATCH: send Authorization with secret=true but no value → preserve.
    bb_err_t rc = run_patch(
        "{\"headers\":[{\"name\":\"Authorization\",\"secret\":true}]}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[512] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, buf, sizeof(buf), "");
    // Original value must still be there.
    TEST_ASSERT_NOT_NULL(strstr(buf, "*Authorization: Bearer original"));
}

void test_bb_sink_http_telemetry_patch_headers_omitted_name_removed(void)
{
    bb_sink_http_telemetry_reset_for_test();
    // Pre-populate with two headers.
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, "X-A: va\nX-B: vb");

    // PATCH: only send X-A → X-B removed.
    bb_err_t rc = run_patch(
        "{\"headers\":[{\"name\":\"X-A\",\"value\":\"new-va\",\"secret\":false}]}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[512] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, buf, sizeof(buf), "");
    TEST_ASSERT_NOT_NULL(strstr(buf, "X-A: new-va"));
    TEST_ASSERT_NULL(strstr(buf, "X-B"));
}

void test_bb_sink_http_telemetry_patch_headers_independent_edit(void)
{
    bb_sink_http_telemetry_reset_for_test();
    // Pre-populate with secret auth + non-secret trace.
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS,
                  "*Authorization: Bearer old\nX-Trace: old-trace");

    // PATCH: update X-Trace, leave Authorization blank (preserve).
    bb_err_t rc = run_patch(
        "{\"headers\":["
        "{\"name\":\"Authorization\",\"secret\":true},"
        "{\"name\":\"X-Trace\",\"value\":\"new-trace\",\"secret\":false}"
        "]}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[512] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, buf, sizeof(buf), "");
    // Secret must be preserved.
    TEST_ASSERT_NOT_NULL(strstr(buf, "*Authorization: Bearer old"));
    // Non-secret must be updated.
    TEST_ASSERT_NOT_NULL(strstr(buf, "X-Trace: new-trace"));
    TEST_ASSERT_NULL(strstr(buf, "old-trace"));
}

// ---------------------------------------------------------------------------
// PATCH: large (>1 KB) tls_ca stored and readable via bb_nv_get_str
// — exercises the heap-allocated scratch buffer (fixes stack overflow on
// the 6144-byte httpd thread when a PEM cert is PATCHed).
// ---------------------------------------------------------------------------

void test_bb_sink_http_telemetry_patch_large_tls_ca_stored_via_heap(void)
{
    bb_sink_http_telemetry_reset_for_test();

    // Build a >1 KB body: header + 20 lines of 64 'B' chars + footer.
    static const char header[] = "-----BEGIN CERTIFICATE-----\n";
    static const char footer[] = "-----END CERTIFICATE-----\n";
    static const char line[]   =
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n";

    // cert_pem ~1300 bytes: 20 × 65 bytes
    char cert_pem[1500];
    int pos = 0;
    pos += (int)snprintf(cert_pem + pos, sizeof(cert_pem) - (size_t)pos,
                         "%s", header);
    for (int i = 0; i < 20; i++) {
        pos += (int)snprintf(cert_pem + pos, sizeof(cert_pem) - (size_t)pos,
                             "%s", line);
    }
    snprintf(cert_pem + pos, sizeof(cert_pem) - (size_t)pos, "%s", footer);

    TEST_ASSERT_TRUE_MESSAGE(strlen(cert_pem) > 1024,
                             "cert_pem must be >1 KB to exercise heap path");

    // Build the JSON patch body with the PEM embedded.
    char *body_json = malloc(strlen(cert_pem) + 64);
    TEST_ASSERT_NOT_NULL(body_json);
    // Escape newlines for JSON.
    char *escaped = malloc(strlen(cert_pem) * 2 + 1);
    TEST_ASSERT_NOT_NULL(escaped);
    size_t j = 0;
    for (size_t i = 0; cert_pem[i]; i++) {
        if (cert_pem[i] == '\n') { escaped[j++] = '\\'; escaped[j++] = 'n'; }
        else                     { escaped[j++] = cert_pem[i]; }
    }
    escaped[j] = '\0';
    sprintf(body_json, "{\"tls_ca\":\"%s\"}", escaped);
    free(escaped);

    bb_err_t rc = run_patch(body_json);
    free(body_json);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    // Verify the value was persisted — use a buffer large enough for the PEM.
    char *stored = malloc(sizeof(cert_pem));
    TEST_ASSERT_NOT_NULL(stored);
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_TLS_CA, stored, sizeof(cert_pem), "");
    TEST_ASSERT_TRUE_MESSAGE(strlen(stored) > 1024,
                             "stored tls_ca must be >1 KB");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(stored, "BEGIN CERTIFICATE"),
                                 "stored tls_ca must contain BEGIN CERTIFICATE");
    free(stored);

    // GET must report ca_set=true.
    cJSON *get_body = run_get();
    TEST_ASSERT_NOT_NULL(get_body);
    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(get_body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_TRUE(cJSON_IsTrue(ca_set));
    cJSON_Delete(get_body);
}
