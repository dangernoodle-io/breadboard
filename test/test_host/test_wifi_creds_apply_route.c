// Host mirror of the "wifi" bb_data ingress binding (B1-1022) that backs
// PATCH /api/wifi in platform/espidf/bb_wifi_http/bb_wifi_http_routes.c.
// That TU is ESP-IDF-only (its component REQUIRES esp_wifi, and it includes
// <esp_wifi.h> unconditionally) and cannot link on host -- see
// test_route_fidelity.c's SKIPPED ROUTES note for /api/scan, the same file.
//
// This file duplicates the binding's field descriptor (key names
// "ssid"/"password", buffer sizing) and gather/apply hooks against the
// production functions, byte-for-byte, and drives them through the REAL
// bb_data_apply() pipeline (JSON parse -> bb_serialize_populate() ->
// apply()) -- exercising the composed route's gather/apply pairing and
// field-name wiring, not just bb_data's own generic unit tests
// (test_bb_data.c). Any edit to the production binding's field layout, key
// names, or apply mode must be mirrored here.
//
// Primary coverage: B1-1022 review finding #1 (MEDIUM) -- the route applies
// via BB_DATA_APPLY_POST (memset0 seed), not BB_DATA_APPLY_PATCH
// (gather-seeded from the last-staged pending creds), specifically so a
// PATCH that omits "password" never resurrects a password staged by a
// PREVIOUS, abandoned attempt targeting a different ssid.

#include "unity.h"

#include "bb_data.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_wifi_pending.h"
#include "fake_nvs_backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Mirrors WIFI_CREDS_APPLY_SSID_BUF/PASS_BUF and bb_wifi_creds_apply_t /
// s_wifi_creds_fields / s_wifi_creds_desc in bb_wifi_http_routes.c.
#define MIRROR_SSID_BUF (BB_WIFI_PENDING_SSID_MAX + 1 + 32)
#define MIRROR_PASS_BUF (BB_WIFI_PENDING_PASS_MAX + 1 + 32)

typedef struct {
    char ssid[MIRROR_SSID_BUF];
    char pass[MIRROR_PASS_BUF];
} mirror_creds_t;

static const bb_serialize_field_t s_mirror_fields[] = {
    { .key = "ssid", .type = BB_TYPE_STR, .offset = offsetof(mirror_creds_t, ssid),
      .max_len = sizeof(((mirror_creds_t *)0)->ssid) },
    { .key = "password", .type = BB_TYPE_STR, .offset = offsetof(mirror_creds_t, pass),
      .max_len = sizeof(((mirror_creds_t *)0)->pass) },
};

static const bb_serialize_desc_t s_mirror_desc = {
    .type_name = "mirror_creds_t",
    .fields    = s_mirror_fields,
    .n_fields  = 2,
    .snap_size = sizeof(mirror_creds_t),
};

// Mirrors wifi_creds_gather()'s current (post-fix) shape: a bare zero-fill.
// bb_data_bind() rejects a NULL gather outright, and production POST-mode
// apply never invokes it -- see that fn's own comment for why it still
// exists.
static bb_err_t mirror_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    memset(dst, 0, sizeof(mirror_creds_t));
    return BB_OK;
}

// Mirrors the PRE-FIX wifi_creds_gather() shape (seeds from the currently
// staged pending creds) -- used ONLY by the contrast fixture below, bound
// under a SEPARATE key so the "wifi" key above always reflects the current,
// fixed production wiring.
static bb_err_t mirror_gather_patch_seed(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    mirror_creds_t *creds = (mirror_creds_t *)dst;
    memset(creds, 0, sizeof(*creds));
    bb_settings_wifi_pending_ssid_get(creds->ssid, sizeof(creds->ssid), NULL);
    bb_settings_wifi_pending_pass_get(creds->pass, sizeof(creds->pass), NULL);
    return BB_OK;
}

// Spy: mirrors wifi_creds_apply()'s validate-then-stage shape, but records
// the scattered snapshot instead of calling bb_wifi_reconfigure() (ESP-IDF
// only; that call's own behavior is exercised by bb_wifi's own tests, not
// this route-composition fixture).
static mirror_creds_t s_captured;
static bool           s_apply_called;

static bb_err_t mirror_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    const mirror_creds_t *creds = (const mirror_creds_t *)snap;

    if (bb_wifi_pending_validate_buf(creds->ssid, sizeof(creds->ssid),
                                      creds->pass, sizeof(creds->pass)) != BB_OK) {
        return BB_ERR_VALIDATION;
    }

    s_captured     = *creds;
    s_apply_called = true;
    return BB_OK;
}

static void reset_all(void)
{
    bb_data_test_reset();

    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);

    static const bb_serialize_format_entry_t entry = {
        .render = bb_serialize_json_render,
        .parse  = bb_serialize_json_parse_bytes,
    };
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry));

    memset(&s_captured, 0, sizeof(s_captured));
    s_apply_called = false;

    bb_data_binding_t b = {
        .key = "wifi", .desc = &s_mirror_desc, .gather = mirror_gather, .apply = mirror_apply,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    // Contrast-only binding (pre-fix gather shape) -- see
    // mirror_gather_patch_seed's own doc.
    bb_data_binding_t b_patch_variant = {
        .key = "wifi_patch_contrast", .desc = &s_mirror_desc,
        .gather = mirror_gather_patch_seed, .apply = mirror_apply,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b_patch_variant));
}

// Mirrors wifi_patch_handler()'s bb_data_apply_req_t wiring.
static bb_err_t run_apply_for_key(const char *key, bb_data_apply_mode_t mode, const char *body)
{
    mirror_creds_t dst;
    char           parse_scratch[3072];
    bb_data_apply_req_t req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = key,
        .mode              = mode,
        .body              = body,
        .body_len          = strlen(body),
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = &dst,
        .dst_scratch_cap   = sizeof(dst),
    };
    return bb_data_apply(&req);
}

// Convenience wrapper against the "wifi" key (the current production
// wiring).
static bb_err_t run_apply(bb_data_apply_mode_t mode, const char *body)
{
    return run_apply_for_key("wifi", mode, body);
}

// B1-1022 review finding #1: a PATCH that only supplies "ssid" (a different
// ssid than whatever was last staged) must NOT resurrect a stale staged
// password. Production wires the route onto BB_DATA_APPLY_POST specifically
// to prevent this.
void test_wifi_creds_apply_post_mode_does_not_reuse_stale_pending_password(void)
{
    reset_all();

    // Simulate an abandoned prior attempt: ssid "networkA" / pass "secretA"
    // left staged as pending.
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("networkA", "secretA"));

    // New request omits "password" entirely and targets a DIFFERENT ssid.
    bb_err_t rc = run_apply(BB_DATA_APPLY_POST, "{\"ssid\":\"networkB\"}");

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_TRUE(s_apply_called);
    TEST_ASSERT_EQUAL_STRING("networkB", s_captured.ssid);
    TEST_ASSERT_EQUAL_STRING("", s_captured.pass);  // NOT "secretA"
}

// Contrast fixture: proves BB_DATA_APPLY_PATCH (the mode this route
// deliberately does NOT use) exhibits exactly the leak finding #1 flagged --
// documents why POST was the fix, not an incidental/unrelated behavior
// change.
void test_wifi_creds_apply_patch_mode_would_reuse_stale_pending_password(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("networkA", "secretA"));

    bb_err_t rc = run_apply_for_key("wifi_patch_contrast", BB_DATA_APPLY_PATCH,
                                     "{\"ssid\":\"networkB\"}");

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_TRUE(s_apply_called);
    TEST_ASSERT_EQUAL_STRING("networkB", s_captured.ssid);
    TEST_ASSERT_EQUAL_STRING("secretA", s_captured.pass);  // the leak PATCH mode has
}

// Field-name wiring: an omitted "ssid" on a POST-mode apply lands as empty
// (memset0 seed) and is rejected -- matches the pre-cutover handler's "ssid
// required" behavior.
void test_wifi_creds_apply_post_mode_missing_ssid_rejected(void)
{
    reset_all();

    bb_err_t rc = run_apply(BB_DATA_APPLY_POST, "{\"password\":\"x\"}");

    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_FALSE(s_apply_called);
}

// Field-name wiring: a full POST-mode replace threads both "ssid" and
// "password" through to apply() untouched.
void test_wifi_creds_apply_post_mode_full_replace(void)
{
    reset_all();

    bb_err_t rc = run_apply(BB_DATA_APPLY_POST, "{\"ssid\":\"networkC\",\"password\":\"pw123\"}");

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_TRUE(s_apply_called);
    TEST_ASSERT_EQUAL_STRING("networkC", s_captured.ssid);
    TEST_ASSERT_EQUAL_STRING("pw123", s_captured.pass);
}

// POST-mode omitted password: an ssid-only request stages an open network,
// same as the pre-cutover handler's default (never partial-update, even
// against a FRESH/no-prior-pending state).
void test_wifi_creds_apply_post_mode_missing_password_is_open_network(void)
{
    reset_all();

    bb_err_t rc = run_apply(BB_DATA_APPLY_POST, "{\"ssid\":\"networkD\"}");

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_TRUE(s_apply_called);
    TEST_ASSERT_EQUAL_STRING("networkD", s_captured.ssid);
    TEST_ASSERT_EQUAL_STRING("", s_captured.pass);
}
