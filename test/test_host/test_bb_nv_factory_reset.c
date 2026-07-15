// Tests for bb_nv_config_factory_reset() and POST /api/factory-reset handler.
//
// B1-260: factory reset capability.
//
// Tests cover:
//   1. Core function: invalidates the shared RTC mirror (host side).
//   2. Route handler: 400 for missing confirm, 400 for wrong confirm, 202 for correct confirm.
//
// bb_nv no longer caches wifi creds (or anything else) in-RAM -- that state
// moved entirely to bb_settings' storage layer (B1: bb_nv creds-cluster
// relocation; hostname/timezone/wifi creds and display/mdns/update-check-
// enabled moved earlier, B1-750/B1-754). On host there is no NVS partition
// to erase, so bb_nv_config_factory_reset()'s host stub does the ONE thing
// left in its scope: invalidate the shared RTC mirror
// (bb_settings_wifi_rtc_mirror_clear(), same backend the ESP_PLATFORM branch
// invalidates after its nvs_flash_erase()) -- see
// platform/espidf/bb_nv/bb_nv.c's comment.

#include "unity.h"
#include "bb_nv.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_storage_rtc.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Core function tests
// ---------------------------------------------------------------------------

// Rollback bite-proof: with the "rtc" backend registered and a live mirror
// armed, factory_reset must invalidate it -- reverting
// bb_nv_config_factory_reset's bb_settings_wifi_rtc_mirror_clear() call (the
// host branch) to a no-op turns this RED.
void test_nv_factory_reset_invalidates_rtc_mirror(void)
{
    bb_storage_test_reset();
    bb_storage_rtc_test_reset();
    bb_storage_rtc_register();

    bb_nv_config_init();
    // Seed the mirror directly (bb_settings_wifi_rtc_mirror_write only needs
    // the "rtc" backend, unlike bb_settings_wifi_set which also needs "nvs"
    // registered -- irrelevant to what this test is proving).
    bb_settings_wifi_rtc_mirror_write("MyNetwork", "hunter2");
    TEST_ASSERT_TRUE(bb_settings_wifi_rtc_mirror_has_creds());

    bb_err_t err = bb_nv_config_factory_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_has_creds());
}

void test_nv_factory_reset_returns_ok_after_reinit(void)
{
    // Idempotent: calling factory_reset twice is safe.
    bb_nv_config_init();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_nv_config_factory_reset());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_nv_config_factory_reset());
}

// Fail-open: no "rtc" backend registered at all -- factory_reset must still
// return BB_OK (the mirror-clear's own error, if any, is swallowed).
void test_nv_factory_reset_ok_when_rtc_backend_unregistered(void)
{
    bb_storage_test_reset();
    // Deliberately NOT calling bb_storage_rtc_register().

    bb_nv_config_init();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_nv_config_factory_reset());
}

// ---------------------------------------------------------------------------
// Route handler tests
// ---------------------------------------------------------------------------

#if CONFIG_BB_NV_FACTORY_RESET

// Helper: call factory_reset_handler with an optional body, return the capture.
static bb_http_host_capture_t run_factory_reset(const char *body)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (body) {
        bb_http_host_capture_set_req_body(body, (int)strlen(body));
    }
    bb_nv_factory_reset_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    return cap;
}

void test_nv_factory_reset_route_no_body_returns_400(void)
{
    bb_nv_config_init();
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
}

void test_nv_factory_reset_route_wrong_confirm_returns_400(void)
{
    bb_nv_config_init();
    bb_http_host_capture_t cap = run_factory_reset("{\"confirm\":\"wrong\"}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(cJSON_IsString(err));
    cJSON_Delete(j);
    bb_http_host_capture_free(&cap);
}

void test_nv_factory_reset_route_missing_confirm_field_returns_400(void)
{
    bb_nv_config_init();
    bb_http_host_capture_t cap = run_factory_reset("{\"other\":\"value\"}");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_nv_factory_reset_route_invalid_json_returns_400(void)
{
    bb_nv_config_init();
    bb_http_host_capture_t cap = run_factory_reset("not-json");
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_nv_factory_reset_route_valid_confirm_returns_202(void)
{
    bb_nv_config_init();

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

void test_nv_factory_reset_route_oversized_body_returns_400(void)
{
    bb_nv_config_init();
    // Build a body longer than 128 bytes.
    char body[160];
    memset(body, 'x', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';

    bb_http_host_capture_t cap = run_factory_reset(body);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);
}

#endif /* CONFIG_BB_NV_FACTORY_RESET */
