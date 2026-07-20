// Tests for the POST /api/diag/factory-reset route handler (B1-960).
//
// Rehomed from test_bb_nv_factory_reset.c's route-handler tests — the route
// now sits on the bb_storage FACADE (bb_storage_erase_all("nvs")) instead of
// bb_nv_config_factory_reset(), so this file drives it against a small FAKE
// "nvs" bb_storage backend registered locally (same fixture shape as
// test_bb_storage_http_routes.c, but tracking erase_all call counts instead
// of namespace/key entries).
//
// Covers:
//   - missing/no body                -> 400
//   - wrong confirm value             -> 400
//   - missing confirm field           -> 400
//   - invalid JSON                    -> 400
//   - oversized body                  -> 400
//   - valid confirm                   -> 202 {"status":"factory_reset_accepted","reboot":true},
//                                        bb_storage_erase_all("nvs") dispatched exactly once
//   - backend without erase_all       -> 501 (BB_ERR_UNSUPPORTED fail-closed, never a silent no-op)
//   - erase_all fails (non-UNSUPPORTED) -> 500 (generic failure branch)
//   - erase_all fails BB_ERR_INVALID_STATE -> 500, NOT the parse-layer's 400
//                                        (B1-859 HIGH regression pin -- see
//                                        fake_erase_all_failing_invalid_state)
//   - valid confirm                   -> RTC creds mirror cleared (B1-935 stale-creds-survive-
//                                        factory-reset class)
//
// The RTC-mirror-clear step (bb_settings_wifi_rtc_mirror_clear(), moved
// verbatim from bb_nv_config_factory_reset's ESP_PLATFORM branch) is called
// unconditionally on host (see the handler's
// `#if !defined(ESP_PLATFORM) || defined(CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP)`
// guard) -- same posture as the deleted bb_nv_config_factory_reset() host
// stub, which called it unconditionally and was covered by
// test_nv_factory_reset_invalidates_rtc_mirror. On device the call stays
// gated on CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP; this file only exercises the host
// branch.

#include "unity.h"
#include "bb_storage_http.h"
#include "bb_data.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"
#include "bb_storage.h"
#include "bb_storage_rtc.h"
#include "bb_settings.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Fake "nvs" bb_storage backend -- tracks erase_all call count instead of
// staging real entries (this route never touches individual keys).
// ---------------------------------------------------------------------------

static int s_erase_all_calls;

static bb_err_t fake_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl; (void)addr; (void)buf; (void)cap;
    *out_len = 0;
    return BB_ERR_NOT_FOUND;
}

static bb_err_t fake_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl; (void)addr; (void)buf; (void)len;
    return BB_OK;
}

static bb_err_t fake_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl; (void)addr;
    return BB_OK;
}

static bool fake_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl; (void)addr;
    return false;
}

static bb_err_t fake_erase_all(void *impl)
{
    (void)impl;
    s_erase_all_calls++;
    return BB_OK;
}

static const bb_storage_vtable_t s_full_vtable = {
    .get       = fake_get,
    .set       = fake_set,
    .erase     = fake_erase,
    .exists    = fake_exists,
    .erase_all = fake_erase_all,
};

// No erase_all — proves the 501/BB_ERR_UNSUPPORTED fail-closed path.
static const bb_storage_vtable_t s_partial_vtable = {
    .get    = fake_get,
    .set    = fake_set,
    .erase  = fake_erase,
    .exists = fake_exists,
};

// erase_all present but fails with a generic (non-BB_ERR_UNSUPPORTED) error
// -- proves the handler's 500 sub-branch (as opposed to the 501
// BB_ERR_UNSUPPORTED branch) is actually reachable and correctly wired.
// There is no BB_ERR_IO code in this codebase (see bb_core.h); BB_ERR_VALIDATION
// stood in as "some real backend failure that isn't unsupported" pre-B1-859,
// but factory_reset_apply()'s own confirm-mismatch check now ALSO returns
// BB_ERR_VALIDATION (a domain-level reject, per bb_data_apply_fn's
// contract) -- reusing it here would collide with that 400 path.
// So this fixture uses BB_ERR_TIMEOUT, distinct from BB_ERR_VALIDATION (400),
// the BB_ERR_PARSE_GRAMMAR/BB_ERR_PARSE_INCOMPLETE parse-layer codes (400),
// and BB_ERR_UNSUPPORTED (501).
static bb_err_t fake_erase_all_failing(void *impl)
{
    (void)impl;
    s_erase_all_calls++;
    return BB_ERR_TIMEOUT;
}

static const bb_storage_vtable_t s_failing_vtable = {
    .get       = fake_get,
    .set       = fake_set,
    .erase     = fake_erase,
    .exists    = fake_exists,
    .erase_all = fake_erase_all_failing,
};

// erase_all failing with BB_ERR_INVALID_STATE specifically -- this is the
// B1-859-era HIGH's regression pin. Before the parse-layer codes were split
// out of BB_ERR_INVALID_STATE (B1-1090/bb_core.h), the route's status
// mapping treated ANY BB_ERR_INVALID_STATE as "invalid JSON" -> 400, so a
// genuinely failed erase (bb_storage_erase_all -> bb_storage_nvs_erase_all
// -> nvs_flash_erase -> esp_partition_erase_range CAN return
// ESP_ERR_INVALID_STATE from the flash/wear-levelling layer) was
// misreported as a 400 client error instead of a 500 server error. Now that
// parse failures use the disjoint BB_ERR_PARSE_GRAMMAR/BB_ERR_PARSE_INCOMPLETE
// codes, a bare BB_ERR_INVALID_STATE from the apply stage falls through to
// the generic 500 branch. Revert either the parse-layer split (bb_core.h) or
// the route's status mapping and this test goes red.
static bb_err_t fake_erase_all_failing_invalid_state(void *impl)
{
    (void)impl;
    s_erase_all_calls++;
    return BB_ERR_INVALID_STATE;
}

static const bb_storage_vtable_t s_failing_invalid_state_vtable = {
    .get       = fake_get,
    .set       = fake_set,
    .erase     = fake_erase,
    .exists    = fake_exists,
    .erase_all = fake_erase_all_failing_invalid_state,
};

// B1-859: the migrated handler drives bb_data_apply(), which requires (a)
// the "factory_reset" key bound (production gather/apply hooks, via
// bb_storage_http_factory_reset_bind_for_test() -- see that fn's own doc for
// why this route can't reuse bb_storage_http_factory_reset_routes_init()
// directly on host) and (b) a registered BB_FORMAT_JSON parse fn (the format
// registry is empty until a consumer registers one -- same posture as
// test_wifi_creds_apply_route.c's reset_all()).
static void bind_bb_data(void)
{
    bb_data_test_reset();

    static const bb_serialize_format_entry_t entry = {
        .render = bb_serialize_json_render,
        .parse  = bb_serialize_json_parse_bytes,
    };
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry));

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_http_factory_reset_bind_for_test());
}

static void reset_all(void)
{
    bb_storage_test_reset();
    s_erase_all_calls = 0;
    bb_storage_register_backend("nvs", &s_full_vtable, NULL);
    bind_bb_data();
}

static void reset_all_no_erase_all(void)
{
    bb_storage_test_reset();
    s_erase_all_calls = 0;
    bb_storage_register_backend("nvs", &s_partial_vtable, NULL);
    bind_bb_data();
}

static void reset_all_failing_erase_all(void)
{
    bb_storage_test_reset();
    s_erase_all_calls = 0;
    bb_storage_register_backend("nvs", &s_failing_vtable, NULL);
    bind_bb_data();
}

static void reset_all_failing_erase_all_invalid_state(void)
{
    bb_storage_test_reset();
    s_erase_all_calls = 0;
    bb_storage_register_backend("nvs", &s_failing_invalid_state_vtable, NULL);
    bind_bb_data();
}

// ---------------------------------------------------------------------------
// Helper: run handler with a JSON body string
// ---------------------------------------------------------------------------

static bb_http_host_capture_t run_factory_reset(const char *body)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (body) {
        bb_http_host_capture_set_req_body(body, (int)strlen(body));
    }
    bb_storage_http_factory_reset_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    return cap;
}

// ---------------------------------------------------------------------------
// Confirm-field validation
// ---------------------------------------------------------------------------

void test_storage_http_factory_reset_no_body_returns_400(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_factory_reset(NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(cJSON_IsString(err));
    cJSON_Delete(j);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(0, s_erase_all_calls);
}

void test_storage_http_factory_reset_wrong_confirm_returns_400(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"wrong\"}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(0, s_erase_all_calls);
}

void test_storage_http_factory_reset_missing_confirm_field_returns_400(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_factory_reset("{\"other\":\"value\"}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(0, s_erase_all_calls);
}

void test_storage_http_factory_reset_invalid_json_returns_400(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_factory_reset("not-json");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(0, s_erase_all_calls);
}

// A truncated body (no closing brace) fails the JSON scan mid-parse rather
// than at the grammar-check-the-whole-thing-up-front stage the "not-json"
// case above exercises -- bb_serialize_json_scan_end() reports this as
// BB_ERR_PARSE_INCOMPLETE (an unterminated object at end-of-input), the
// disjoint parse-layer code (bb_core.h, B1-1090) distinct from both
// BB_ERR_VALIDATION and BB_ERR_INVALID_STATE. The route maps it to 400
// (parity with the pre-B1-859 bb_json handler, which returned 400 "invalid
// JSON" for any unparseable body regardless of cause) -- this pins that a
// truncated body doesn't regress to a 500. Removing BB_ERR_PARSE_INCOMPLETE
// from the route's 400 list turns this test red.
void test_storage_http_factory_reset_truncated_body_returns_400(void)
{
    reset_all();
    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"factory-reset\"");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(0, s_erase_all_calls);
}

// Bounded body read: a body longer than the route's cap is rejected outright
// (400), never silently truncated.
void test_storage_http_factory_reset_oversized_body_returns_400(void)
{
    reset_all();
    char body[160];
    memset(body, 'x', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';

    bb_http_host_capture_t cap = run_factory_reset(body);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(0, s_erase_all_calls);
}

// A duplicate "confirm" key pins first-match-wins ingress semantics --
// bb_serialize_json_tok_obj_get() (the lookup every bb_data-fed route rides)
// returns the FIRST matching key's token, so the earlier "wrong" value wins
// over the later valid one and the request is rejected 400, erase never
// invoked. This is not factory-reset-specific behavior; it pins the shared
// contract every bb_data route depends on.
void test_storage_http_factory_reset_duplicate_confirm_key_first_match_wins(void)
{
    reset_all();
    bb_http_host_capture_t cap =
        run_factory_reset("{\"confirm\":\"wrong\",\"confirm\":\"factory-reset\"}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(0, s_erase_all_calls);
}

// ---------------------------------------------------------------------------
// Valid confirm — 202 + erase_all dispatched
// ---------------------------------------------------------------------------

void test_storage_http_factory_reset_valid_confirm_returns_202(void)
{
    reset_all();

    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"factory-reset\"}");
    TEST_ASSERT_EQUAL_INT(202, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);

    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(j, "202 body is not valid JSON");

    cJSON *status = cJSON_GetObjectItemCaseSensitive(j, "status");
    TEST_ASSERT_NOT_NULL(status);
    TEST_ASSERT_TRUE(cJSON_IsString(status));
    TEST_ASSERT_EQUAL_STRING("factory_reset_accepted", cJSON_GetStringValue(status));

    cJSON *reboot = cJSON_GetObjectItemCaseSensitive(j, "reboot");
    TEST_ASSERT_NOT_NULL(reboot);
    TEST_ASSERT_TRUE(cJSON_IsBool(reboot));
    TEST_ASSERT_TRUE(cJSON_IsTrue(reboot));

    cJSON_Delete(j);
    bb_http_host_capture_free(&cap);
}

void test_storage_http_factory_reset_erase_all_dispatched_to_backend(void)
{
    reset_all();

    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"factory-reset\"}");
    TEST_ASSERT_EQUAL_INT(202, cap.status);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_EQUAL_INT(1, s_erase_all_calls);
}

// ---------------------------------------------------------------------------
// RTC creds mirror is cleared on a successful factory-reset (B1-935 stale-
// creds-survive-factory-reset class). Reverting the handler's mirror-clear
// call to a no-op (or re-narrowing its guard to device-only) turns this RED.
// ---------------------------------------------------------------------------

void test_storage_http_factory_reset_clears_rtc_creds_mirror(void)
{
    reset_all();
    bb_storage_rtc_test_reset();
    bb_storage_rtc_register();

    bb_settings_wifi_rtc_mirror_write("MyNetwork", "hunter2");
    TEST_ASSERT_TRUE(bb_settings_wifi_rtc_mirror_has_creds());

    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"factory-reset\"}");
    TEST_ASSERT_EQUAL_INT(202, cap.status);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_has_creds());
}

// ---------------------------------------------------------------------------
// Fail-closed: a backend that leaves erase_all NULL surfaces 501, never a
// silent no-op on a destructive request.
// ---------------------------------------------------------------------------

void test_storage_http_factory_reset_backend_without_erase_all_returns_501(void)
{
    reset_all_no_erase_all();

    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"factory-reset\"}");
    TEST_ASSERT_EQUAL_INT(501, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(cJSON_IsString(err));
    cJSON_Delete(j);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(0, s_erase_all_calls);
}

// ---------------------------------------------------------------------------
// Generic (non-UNSUPPORTED) erase_all failure surfaces 500 -- distinct from
// the 501 fail-closed path above.
// ---------------------------------------------------------------------------

void test_storage_http_factory_reset_erase_all_generic_failure_returns_500(void)
{
    reset_all_failing_erase_all();

    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"factory-reset\"}");
    TEST_ASSERT_EQUAL_INT(500, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(cJSON_IsString(err));
    cJSON_Delete(j);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(1, s_erase_all_calls);
}

// Regression pin for the B1-859 HIGH: an apply-stage BB_ERR_INVALID_STATE
// (a genuinely failed erase, e.g. esp_partition_erase_range returning
// ESP_ERR_INVALID_STATE from the flash/wear-levelling layer) must surface
// as 500, never the parse-layer's 400 "invalid JSON" -- those used to alias
// the same code before BB_ERR_PARSE_GRAMMAR/BB_ERR_PARSE_INCOMPLETE were
// split out (bb_core.h, B1-1090). Before that split, this exact scenario
// would have returned 400.
void test_storage_http_factory_reset_erase_all_invalid_state_returns_500(void)
{
    reset_all_failing_erase_all_invalid_state();

    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"factory-reset\"}");
    TEST_ASSERT_EQUAL_INT(500, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(cJSON_IsString(err));
    cJSON_Delete(j);
    bb_http_host_capture_free(&cap);
    TEST_ASSERT_EQUAL_INT(1, s_erase_all_calls);
}

// ---------------------------------------------------------------------------
// B1-859: exercise the bound "factory_reset" key's gather stub directly.
// Production always applies via BB_DATA_APPLY_POST (the route hardcodes
// this), so factory_reset_gather() is never invoked by the HTTP handler --
// same "exists only to satisfy bb_data_bind()'s non-NULL-gather invariant"
// posture as wifi_creds_gather() in bb_wifi_http_routes.c. This drives the
// SAME bound key through bb_data_apply() directly in BB_DATA_APPLY_PATCH
// mode (bypassing the HTTP handler, which never uses this mode) purely to
// reach the gather stub's memset0 body -- the end-to-end outcome (a valid
// "factory-reset" confirm, so PATCH's gather-then-populate-overwrite still
// lands on the same confirm value POST would have) is otherwise identical
// to the handler's own success path.
// ---------------------------------------------------------------------------

void test_storage_http_factory_reset_gather_stub_reachable_via_patch_mode(void)
{
    reset_all();

    char body[]           = "{\"confirm\":\"factory-reset\"}";
    char dst_scratch[64];
    char parse_scratch[3072];
    bb_data_apply_req_t req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "factory_reset",
        .mode              = BB_DATA_APPLY_PATCH,
        .body              = body,
        .body_len          = strlen(body),
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };

    TEST_ASSERT_EQUAL(BB_OK, bb_data_apply(&req));
    TEST_ASSERT_EQUAL_INT(1, s_erase_all_calls);
}
