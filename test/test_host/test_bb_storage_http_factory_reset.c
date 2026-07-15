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
// stands in as "some real backend failure that isn't unsupported".
static bb_err_t fake_erase_all_failing(void *impl)
{
    (void)impl;
    s_erase_all_calls++;
    return BB_ERR_VALIDATION;
}

static const bb_storage_vtable_t s_failing_vtable = {
    .get       = fake_get,
    .set       = fake_set,
    .erase     = fake_erase,
    .exists    = fake_exists,
    .erase_all = fake_erase_all_failing,
};

static void reset_all(void)
{
    bb_storage_test_reset();
    s_erase_all_calls = 0;
    bb_storage_register_backend("nvs", &s_full_vtable, NULL);
}

static void reset_all_no_erase_all(void)
{
    bb_storage_test_reset();
    s_erase_all_calls = 0;
    bb_storage_register_backend("nvs", &s_partial_vtable, NULL);
}

static void reset_all_failing_erase_all(void)
{
    bb_storage_test_reset();
    s_erase_all_calls = 0;
    bb_storage_register_backend("nvs", &s_failing_vtable, NULL);
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
