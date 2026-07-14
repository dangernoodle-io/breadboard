// Tests for bb_nv_config_factory_reset() and POST /api/factory-reset handler.
//
// B1-260: factory reset capability.
//
// Tests cover:
//   1. Core function: clears in-memory config, invalidates mirror (host side).
//   2. Route handler: 400 for missing confirm, 400 for wrong confirm, 202 for correct confirm.
//
// On host, the RTC mirror does not exist; we verify mirror-invalidation by
// checking that config fields return to defaults (clears ssid). Hostname,
// timezone, and display/mdns/update-check-enabled all moved to bb_settings
// (B1-754/B1-750) and are no longer part of bb_nv's own factory-reset scope
// (bb_nv's host stub only zeroes its own legacy s_config -- see
// bb_nv_config_factory_reset's comment in platform/espidf/bb_nv/bb_nv.c).
// The mirror-invalidation logic path for the RTC region is covered by the
// ESP-IDF impl at compile time; on host the #if CONFIG_BB_NV_CREDS_RTC_BACKUP
// path does not execute, but the surrounding code is exercised.

#include "unity.h"
#include "bb_nv.h"
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

void test_nv_factory_reset_clears_wifi_ssid(void)
{
    bb_nv_config_init();
    // Host: can't actually set wifi creds via API (ESP-only), but the in-memory
    // s_config fields are initialised to "" by bb_nv_config_init, and
    // bb_nv_config_factory_reset re-zeros them.
    bb_err_t err = bb_nv_config_factory_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("", bb_nv_config_wifi_ssid());
    TEST_ASSERT_EQUAL_STRING("", bb_nv_config_wifi_pass());
}

void test_nv_factory_reset_returns_ok_after_reinit(void)
{
    // Idempotent: calling factory_reset twice is safe.
    bb_nv_config_init();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_nv_config_factory_reset());
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
