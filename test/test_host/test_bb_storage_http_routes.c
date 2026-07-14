// Tests for the DELETE /api/diag/storage route handler (B1-757).
//
// Rehomed from test_bb_nv_delete_routes.c's route-handler tests — the route
// now sits on the bb_storage FACADE (backend-agnostic), not bb_nv, so this
// file drives it against small FAKE bb_storage backends registered locally,
// namespace-AND-key-scoped (unlike the shared test/test_host/
// fake_nvs_backend.h fixture, which is deliberately global-key-only and thus
// unsuitable here: this file's array/leaves-other-ns-intact and
// explicit-backend-selection tests specifically prove namespace/backend
// scoping, which a global-key fake cannot distinguish).
//
// Covers:
//   - namespace string, no key       → clears namespace; 200 {"deleted":["ns"]}
//   - namespace string + key         → clears single key; 200 {"deleted":["ns"],"key":"k"}
//   - namespace array                → clears each; 200 {"deleted":[...]}
//   - array leaves other namespaces  → intact (namespace scoping)
//   - missing confirm / confirm:false → 412
//   - wifi-creds namespace without wipe_wifi → 412 (bb_settings_ns_is_wifi_creds gate)
//   - array namespace incl. wifi-creds ns, WITH wipe_wifi → 200, both erased
//   - key + array namespace          → 400 (ambiguous)
//   - missing namespace field        → 400
//   - no body                        → 400
//   - unknown backend                → 500 (bb_storage_erase_namespace -> BB_ERR_NOT_FOUND)
//   - backend without erase_namespace → 501 (vtable erase_namespace == NULL -> BB_ERR_UNSUPPORTED)
//   - explicit "backend" field        → selects the right backend, leaves others untouched
//   - erasing a key that never existed → still 200 (bb_storage_erase is idempotent)

#include "unity.h"
#include "bb_storage_http.h"
#include "bb_storage.h"
#include "bb_settings.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Fake namespace+key-scoped bb_storage backends
// ---------------------------------------------------------------------------

#define FAKE_MAX_ENTRIES 16
#define FAKE_NS_MAX      32
#define FAKE_KEY_MAX     32
#define FAKE_VALUE_MAX   64

typedef struct {
    bool    used;
    char    ns[FAKE_NS_MAX];
    char    key[FAKE_KEY_MAX];
    size_t  len;
    uint8_t value[FAKE_VALUE_MAX];
} fake_entry_t;

// Two independent tables ("nvs", "alt") registered under the FULL vtable
// (get/set/erase/exists/erase_namespace), plus a third ("noerasens")
// registered under a PARTIAL vtable that omits erase_namespace — proves the
// facade's NULL-optional-op dispatch (BB_ERR_UNSUPPORTED) surfaces as 501,
// never a silent no-op on a destructive request.
enum { TBL_NVS = 0, TBL_ALT = 1, TBL_NOERASENS = 2, TBL_COUNT };
static fake_entry_t s_fake[TBL_COUNT][FAKE_MAX_ENTRIES];

static fake_entry_t *table_for(void *impl)
{
    return s_fake[(int)(intptr_t)impl];
}

static fake_entry_t *fake_find(fake_entry_t *tbl, const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) return NULL;
    for (int i = 0; i < FAKE_MAX_ENTRIES; i++) {
        if (tbl[i].used && strcmp(tbl[i].ns, ns) == 0 && strcmp(tbl[i].key, key) == 0) {
            return &tbl[i];
        }
    }
    return NULL;
}

static bb_err_t fake_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    fake_entry_t *e = fake_find(table_for(impl), addr->ns_or_dir, addr->key);
    if (e == NULL) return BB_ERR_NOT_FOUND;
    *out_len = e->len;
    if (cap > 0) {
        size_t copy_len = e->len < cap ? e->len : cap;
        memcpy(buf, e->value, copy_len);
    }
    return BB_OK;
}

static bb_err_t fake_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    if (len > FAKE_VALUE_MAX) return BB_ERR_NO_SPACE;
    fake_entry_t *tbl = table_for(impl);
    fake_entry_t *e = fake_find(tbl, addr->ns_or_dir, addr->key);
    if (e == NULL) {
        for (int i = 0; i < FAKE_MAX_ENTRIES; i++) {
            if (!tbl[i].used) { e = &tbl[i]; break; }
        }
        if (e == NULL) return BB_ERR_NO_SPACE;
        strncpy(e->ns, addr->ns_or_dir, sizeof(e->ns) - 1);
        e->ns[sizeof(e->ns) - 1] = '\0';
        strncpy(e->key, addr->key, sizeof(e->key) - 1);
        e->key[sizeof(e->key) - 1] = '\0';
        e->used = true;
    }
    if (len > 0) memcpy(e->value, buf, len);
    e->len = len;
    return BB_OK;
}

static bb_err_t fake_erase(void *impl, const bb_storage_addr_t *addr)
{
    fake_entry_t *e = fake_find(table_for(impl), addr->ns_or_dir, addr->key);
    if (e != NULL) memset(e, 0, sizeof(*e));
    return BB_OK;  // idempotent: erasing an absent key is still BB_OK
}

static bool fake_exists(void *impl, const bb_storage_addr_t *addr)
{
    return fake_find(table_for(impl), addr->ns_or_dir, addr->key) != NULL;
}

static bb_err_t fake_erase_namespace(void *impl, const char *ns_or_dir)
{
    fake_entry_t *tbl = table_for(impl);
    for (int i = 0; i < FAKE_MAX_ENTRIES; i++) {
        if (tbl[i].used && ns_or_dir != NULL && strcmp(tbl[i].ns, ns_or_dir) == 0) {
            memset(&tbl[i], 0, sizeof(tbl[i]));
        }
    }
    return BB_OK;
}

static const bb_storage_vtable_t s_full_vtable = {
    .get             = fake_get,
    .set             = fake_set,
    .erase           = fake_erase,
    .exists          = fake_exists,
    .erase_namespace = fake_erase_namespace,
};

// No erase_namespace — proves the 501 path.
static const bb_storage_vtable_t s_partial_vtable = {
    .get    = fake_get,
    .set    = fake_set,
    .erase  = fake_erase,
    .exists = fake_exists,
};

static void reset_all(void)
{
    bb_storage_test_reset();
    memset(s_fake, 0, sizeof(s_fake));
    bb_storage_register_backend("nvs", &s_full_vtable, (void *)(intptr_t)TBL_NVS);
    bb_storage_register_backend("alt", &s_full_vtable, (void *)(intptr_t)TBL_ALT);
    bb_storage_register_backend("noerasens", &s_partial_vtable, (void *)(intptr_t)TBL_NOERASENS);
}

static bool fake_exists_pub(const char *backend, const char *ns, const char *key)
{
    bb_storage_addr_t addr = { .backend = backend, .ns_or_dir = ns, .key = key };
    return bb_storage_exists(&addr);
}

static void fake_set_pub(const char *backend, const char *ns, const char *key, const char *value)
{
    bb_storage_addr_t addr = { .backend = backend, .ns_or_dir = ns, .key = key };
    bb_storage_set(&addr, value, strlen(value));
}

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
    bb_storage_http_delete_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    return cap;
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — missing / no body
// ---------------------------------------------------------------------------

void test_storage_delete_no_body_returns_400(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_handler_body(NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — confirm guards
// ---------------------------------------------------------------------------

void test_storage_delete_missing_confirm_returns_412(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_handler_body("{\"namespace\":\"test_ns\"}");
    TEST_ASSERT_EQUAL_INT(412, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_storage_delete_confirm_false_returns_412(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"test_ns\",\"confirm\":false}");
    TEST_ASSERT_EQUAL_INT(412, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — missing namespace
// ---------------------------------------------------------------------------

void test_storage_delete_missing_namespace_returns_400(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_handler_body("{\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — default backend ("nvs"), namespace string
// ---------------------------------------------------------------------------

void test_storage_delete_ns_string_clears_namespace_returns_200(void)
{
    reset_all();
    fake_set_pub("nvs", "bb_mqtt", "broker", "mqtt://example.com");
    TEST_ASSERT_TRUE(fake_exists_pub("nvs", "bb_mqtt", "broker"));

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

    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_mqtt", "broker"));
    bb_http_host_capture_free(&cap);
}

void test_storage_delete_ns_string_with_key_clears_key_returns_200(void)
{
    reset_all();
    fake_set_pub("nvs", "bb_mqtt", "broker", "mqtt://example.com");
    fake_set_pub("nvs", "bb_mqtt", "qos",    "1");
    TEST_ASSERT_TRUE(fake_exists_pub("nvs", "bb_mqtt", "broker"));
    TEST_ASSERT_TRUE(fake_exists_pub("nvs", "bb_mqtt", "qos"));

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_mqtt\",\"key\":\"broker\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);

    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *key_field = cJSON_GetObjectItemCaseSensitive(j, "key");
    TEST_ASSERT_NOT_NULL(key_field);
    TEST_ASSERT_EQUAL_STRING("broker", key_field->valuestring);
    cJSON_Delete(j);

    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_mqtt", "broker"));
    TEST_ASSERT_TRUE(fake_exists_pub("nvs", "bb_mqtt", "qos"));
    bb_http_host_capture_free(&cap);
}

void test_storage_delete_ns_array_clears_each_returns_200(void)
{
    reset_all();
    fake_set_pub("nvs", "bb_mqtt",      "broker",  "mqtt://example.com");
    fake_set_pub("nvs", "bb_scratch_a", "base",    "https://example.com");
    fake_set_pub("nvs", "bb_scratch_b", "enabled", "1");

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\",\"bb_scratch_a\",\"bb_scratch_b\"],\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);

    cJSON *j = cJSON_Parse(cap.body);
    cJSON *del = cJSON_GetObjectItemCaseSensitive(j, "deleted");
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(del));
    cJSON_Delete(j);

    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_mqtt",      "broker"));
    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_scratch_a", "base"));
    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_scratch_b", "enabled"));
    bb_http_host_capture_free(&cap);
}

void test_storage_delete_ns_array_leaves_other_ns_intact(void)
{
    reset_all();
    fake_set_pub("nvs", "bb_other", "hostname", "test-host");
    fake_set_pub("nvs", "bb_mqtt",  "broker",   "mqtt://example.com");

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\"],\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);

    TEST_ASSERT_TRUE(fake_exists_pub("nvs", "bb_other", "hostname"));
    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_mqtt", "broker"));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — wifi-creds namespace guard (bb_settings-owned)
// ---------------------------------------------------------------------------

void test_storage_delete_wifi_creds_ns_without_wipe_wifi_returns_412(void)
{
    reset_all();
    TEST_ASSERT_TRUE(bb_settings_ns_is_wifi_creds("bb_cfg"));

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_cfg\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(412, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_storage_delete_wifi_creds_ns_with_wipe_wifi_returns_200(void)
{
    reset_all();
    fake_set_pub("nvs", "bb_cfg", "wifi_ssid", "MyNet");
    TEST_ASSERT_TRUE(fake_exists_pub("nvs", "bb_cfg", "wifi_ssid"));

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_cfg\",\"confirm\":true,\"wipe_wifi\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_cfg", "wifi_ssid"));
    bb_http_host_capture_free(&cap);
}

void test_storage_delete_array_with_wifi_creds_ns_no_wipe_wifi_returns_412(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\",\"bb_cfg\"],\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(412, cap.status);
    bb_http_host_capture_free(&cap);
}

// Array namespace CONTAINING the wifi-creds namespace, WITH the guard
// satisfied ("wipe_wifi":true) -- the compound branch the array-namespace +
// wifi-creds-guard code introduced that neither sibling test above exercises
// (one is array+no-wipe_wifi->412, the other is single-string+wipe_wifi->200).
// Must BITE: assert BOTH namespaces are actually erased, not just a 200
// status.
void test_storage_delete_array_with_wifi_creds_ns_wipe_wifi_returns_200(void)
{
    reset_all();
    fake_set_pub("nvs", "bb_mqtt", "broker",     "mqtt://example.com");
    fake_set_pub("nvs", "bb_cfg",  "wifi_ssid",  "MyNet");

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\",\"bb_cfg\"],\"confirm\":true,\"wipe_wifi\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);

    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_mqtt", "broker"));
    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_cfg",  "wifi_ssid"));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — oversized "backend"/"key" is REJECTED (400),
// never silently truncated to a prefix that could collide with a real
// registered name (see get_string_field_checked() in the route handler).
// ---------------------------------------------------------------------------

void test_storage_delete_oversized_backend_returns_400(void)
{
    reset_all();
    // BB_STORAGE_HTTP_BACKEND_MAX is 16; this value is 20 chars, so it
    // truncates to "nvsnvsnvsnvsnvs" (15 chars) if the length check is
    // dropped -- assert it is rejected outright instead.
    bb_http_host_capture_t cap = run_handler_body(
        "{\"backend\":\"nvsnvsnvsnvsnvsnvsnv\",\"namespace\":\"bb_mqtt\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_storage_delete_oversized_key_returns_400(void)
{
    reset_all();
    // key buffer is 16 bytes; this value is 20 chars.
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_mqtt\",\"key\":\"aaaaaaaaaaaaaaaaaaaa\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — key + array namespace → 400 (ambiguous)
// ---------------------------------------------------------------------------

void test_storage_delete_key_with_array_ns_returns_400(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":[\"bb_mqtt\",\"bb_scratch_a\"],\"key\":\"broker\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — production-path perturbations (BITE tests)
// ---------------------------------------------------------------------------

// Wrong/unknown backend: bb_storage_erase_namespace returns BB_ERR_NOT_FOUND
// (no backend registered under that name) -> handler surfaces 500, never a
// silent success.
void test_storage_delete_unknown_backend_returns_500(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_handler_body(
        "{\"backend\":\"does_not_exist\",\"namespace\":\"bb_mqtt\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(500, cap.status);
    bb_http_host_capture_free(&cap);
}

// A backend whose vtable omits erase_namespace (NULL) must surface
// BB_ERR_UNSUPPORTED as a proper HTTP error (501), never silently succeed on
// a destructive namespace-wipe request.
void test_storage_delete_backend_without_erase_namespace_returns_501(void)
{
    reset_all();
    fake_set_pub("noerasens", "bb_scratch", "k", "v");

    bb_http_host_capture_t cap = run_handler_body(
        "{\"backend\":\"noerasens\",\"namespace\":\"bb_scratch\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(501, cap.status);
    // Unsupported op must NOT have erased anything.
    TEST_ASSERT_TRUE(fake_exists_pub("noerasens", "bb_scratch", "k"));
    bb_http_host_capture_free(&cap);
}

// Explicit "backend" field selects the RIGHT backend -- the same namespace
// name on a different backend must be untouched (the whole reason this
// route moved off a single hardwired "nvs" backend).
void test_storage_delete_explicit_backend_selects_correct_backend(void)
{
    reset_all();
    fake_set_pub("nvs", "shared_ns", "k", "nvs-value");
    fake_set_pub("alt", "shared_ns", "k", "alt-value");

    bb_http_host_capture_t cap = run_handler_body(
        "{\"backend\":\"alt\",\"namespace\":\"shared_ns\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);

    TEST_ASSERT_FALSE(fake_exists_pub("alt", "shared_ns", "k"));
    TEST_ASSERT_TRUE(fake_exists_pub("nvs", "shared_ns", "k"));
    bb_http_host_capture_free(&cap);
}

// Erasing a key that was never set is still idempotent success (mirrors
// bb_storage_erase's documented contract) -- a "key that doesn't exist"
// production-path perturbation.
void test_storage_delete_key_not_found_still_returns_200(void)
{
    reset_all();
    TEST_ASSERT_FALSE(fake_exists_pub("nvs", "bb_mqtt", "never_set"));

    bb_http_host_capture_t cap = run_handler_body(
        "{\"namespace\":\"bb_mqtt\",\"key\":\"never_set\",\"confirm\":true}");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);
}
