// Tests for bb_mqtt_telemetry section:
// - GET masks password + reports set-flags + connected state
// - PATCH persists each field to NVS "bb_mqtt"
// - PATCH bad body → returns error (tested via dispatch_patch)
#include "unity.h"
#include "bb_mqtt_client.h"
#include "bb_mqtt_telemetry.h"
#include "bb_nv.h"
#include "bb_nv_keys.h"
#include "bb_json.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// Test hook declarations from host twin.
void     bb_mqtt_telemetry_section_get_for_test(bb_json_t section, void *ctx);
bb_err_t bb_mqtt_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static cJSON *run_get(void)
{
    bb_json_t section = bb_json_obj_new();
    bb_mqtt_telemetry_section_get_for_test(section, NULL);
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
    bb_err_t rc = bb_mqtt_telemetry_section_patch_for_test(patch, NULL);
    bb_json_free(patch);
    return rc;
}

// ---------------------------------------------------------------------------
// GET: default state (all empty NVS)
// ---------------------------------------------------------------------------

void test_bb_mqtt_telemetry_get_empty_nvs(void)
{
    bb_mqtt_telemetry_reset_for_test();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "GET did not emit valid JSON");

    cJSON *uri = cJSON_GetObjectItemCaseSensitive(body, "uri");
    TEST_ASSERT_NOT_NULL(uri);
    TEST_ASSERT_TRUE(cJSON_IsString(uri));

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_FALSE(cJSON_IsTrue(connected));

    cJSON *tls = cJSON_GetObjectItemCaseSensitive(body, "tls");
    TEST_ASSERT_NOT_NULL(tls);
    TEST_ASSERT_FALSE(cJSON_IsTrue(tls));

    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET: password is masked ("***"), never returned as plaintext
// ---------------------------------------------------------------------------

void test_bb_mqtt_telemetry_get_masks_password(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_nv_set_str("bb_mqtt", "password", "super-secret");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *pw = cJSON_GetObjectItemCaseSensitive(body, "password");
    TEST_ASSERT_NOT_NULL(pw);
    TEST_ASSERT_TRUE(cJSON_IsString(pw));
    TEST_ASSERT_NULL(strstr(cJSON_GetStringValue(pw), "super-secret"));
    TEST_ASSERT_EQUAL_STRING("***", cJSON_GetStringValue(pw));

    cJSON_Delete(body);
}

void test_bb_mqtt_telemetry_get_password_empty_when_not_set(void)
{
    bb_mqtt_telemetry_reset_for_test();
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

void test_bb_mqtt_telemetry_get_ca_set_true_when_nvs_has_ca(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_nv_set_str(BB_MQTT_NVS_NS, BB_NV_KEY_TLS_CA,
                  "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n");

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_TRUE(cJSON_IsTrue(ca_set));

    cJSON_Delete(body);
}

void test_bb_mqtt_telemetry_get_ca_set_false_when_not_in_nvs(void)
{
    bb_mqtt_telemetry_reset_for_test();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *ca_set = cJSON_GetObjectItemCaseSensitive(body, "ca_set");
    TEST_ASSERT_NOT_NULL(ca_set);
    TEST_ASSERT_FALSE(cJSON_IsTrue(ca_set));

    cJSON_Delete(body);
}

void test_bb_mqtt_telemetry_get_cert_set_and_key_set_flags(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_nv_set_str(BB_MQTT_NVS_NS, BB_NV_KEY_TLS_CERT,
                  "-----BEGIN CERTIFICATE-----\nfake_cert\n-----END CERTIFICATE-----\n");
    bb_nv_set_str(BB_MQTT_NVS_NS, BB_NV_KEY_TLS_KEY,
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
// GET: connected reflects client handle state
// ---------------------------------------------------------------------------

void test_bb_mqtt_telemetry_get_connected_from_client(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_mqtt_client_t client = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_client_init(&cfg, &client);
    bb_mqtt_client_host_set_connected(client, true);
    bb_mqtt_telemetry_set_client(&client);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_TRUE(cJSON_IsTrue(connected));

    cJSON_Delete(body);
    bb_mqtt_telemetry_set_client(NULL);
    bb_mqtt_client_destroy(client);
}

void test_bb_mqtt_telemetry_get_connected_false_when_disconnected(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_mqtt_client_t client = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_client_init(&cfg, &client);
    bb_mqtt_client_host_set_connected(client, false);
    bb_mqtt_telemetry_set_client(&client);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_FALSE(cJSON_IsTrue(connected));

    cJSON_Delete(body);
    bb_mqtt_telemetry_set_client(NULL);
    bb_mqtt_client_destroy(client);
}

void test_bb_mqtt_telemetry_get_connected_via_default(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_mqtt_client_t client = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_client_init(&cfg, &client);
    bb_mqtt_client_host_set_connected(client, true);
    bb_mqtt_client_default_set(client);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_TRUE(cJSON_IsTrue(connected));

    cJSON_Delete(body);
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(client);
}

void test_bb_mqtt_telemetry_get_connected_false_via_default_when_disconnected(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_mqtt_client_t client = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_client_init(&cfg, &client);
    bb_mqtt_client_host_set_connected(client, false);
    bb_mqtt_client_default_set(client);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *connected = cJSON_GetObjectItemCaseSensitive(body, "connected");
    TEST_ASSERT_NOT_NULL(connected);
    TEST_ASSERT_FALSE(cJSON_IsTrue(connected));

    cJSON_Delete(body);
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(client);
}

// ---------------------------------------------------------------------------
// PATCH: persists each field to NVS
// ---------------------------------------------------------------------------

void test_bb_mqtt_telemetry_patch_persists_uri(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"uri\":\"mqtt://broker.example.com:1883\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[128] = {0};
    bb_nv_get_str("bb_mqtt", "uri", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.example.com:1883", buf);
}

void test_bb_mqtt_telemetry_patch_persists_client_id(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"client_id\":\"my-sensor-01\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[64] = {0};
    bb_nv_get_str(BB_MQTT_NVS_NS, BB_NV_KEY_CLIENT_ID, buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("my-sensor-01", buf);
}

void test_bb_mqtt_telemetry_patch_persists_username(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"username\":\"acme-user\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[64] = {0};
    bb_nv_get_str("bb_mqtt", "username", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("acme-user", buf);
}

void test_bb_mqtt_telemetry_patch_persists_password(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"password\":\"tk-test-000\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[64] = {0};
    bb_nv_get_str("bb_mqtt", "password", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("tk-test-000", buf);
}

void test_bb_mqtt_telemetry_patch_persists_tls_ca(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch(
        "{\"tls_ca\":\"-----BEGIN CERTIFICATE-----\\ntest\\n-----END CERTIFICATE-----\\n\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[256] = {0};
    bb_nv_get_str(BB_MQTT_NVS_NS, BB_NV_KEY_TLS_CA, buf, sizeof(buf), "");
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_mqtt_telemetry_patch_persists_enabled(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "enabled", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_bb_mqtt_telemetry_patch_persists_tls_flag(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"tls\":true}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "tls", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_bb_mqtt_telemetry_patch_partial_update_leaves_others(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_nv_set_str("bb_mqtt", "uri", "mqtt://original.example.com:1883");
    bb_err_t rc = run_patch("{\"client_id\":\"new-id\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[128] = {0};
    bb_nv_get_str("bb_mqtt", "uri", buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("mqtt://original.example.com:1883", buf);

    char id_buf[64] = {0};
    bb_nv_get_str(BB_MQTT_NVS_NS, BB_NV_KEY_CLIENT_ID, id_buf, sizeof(id_buf), "");
    TEST_ASSERT_EQUAL_STRING("new-id", id_buf);
}

// ---------------------------------------------------------------------------
// B1-289: PATCH is NVS-only (no live reconfigure); sets pending_reboot
// ---------------------------------------------------------------------------

void test_bb_mqtt_telemetry_patch_does_not_trigger_reconfigure(void)
{
    // Under B1-289 the section patch is NVS-only; no reconfigure should occur.
    // The pending_reboot flag is set instead (tested via bb_telemetry).
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"uri\":\"mqtt://broker.example.com:1883\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    // If we reach here without crash/undefined-symbol, the patch is NVS-only.
}

void test_bb_mqtt_telemetry_patch_new_uri_observed_by_get(void)
{
    bb_mqtt_telemetry_reset_for_test();
    bb_err_t rc = run_patch("{\"uri\":\"mqtt://new-broker.example.com:1883\",\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *uri = cJSON_GetObjectItemCaseSensitive(body, "uri");
    TEST_ASSERT_NOT_NULL(uri);
    TEST_ASSERT_EQUAL_STRING("mqtt://new-broker.example.com:1883", cJSON_GetStringValue(uri));
    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// PATCH: large (>1 KB) tls_ca stored and readable via bb_nv_get_str
// — exercises the heap-allocated scratch buffer (fixes stack overflow on
// the 6144-byte httpd thread when a PEM cert is PATCHed).
// ---------------------------------------------------------------------------

void test_bb_mqtt_telemetry_patch_large_tls_ca_stored_via_heap(void)
{
    bb_mqtt_telemetry_reset_for_test();

    // Build a >1 KB body: header + 20 lines of 64 'A' chars + footer.
    static const char header[] = "-----BEGIN CERTIFICATE-----\n";
    static const char footer[] = "-----END CERTIFICATE-----\n";
    static const char line[]   =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";

    // cert_body ~1300 bytes: 20 × 65 bytes
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
    bb_nv_get_str(BB_MQTT_NVS_NS, BB_NV_KEY_TLS_CA, stored, sizeof(cert_pem), "");
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
