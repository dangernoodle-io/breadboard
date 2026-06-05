#include "unity.h"
#include "bb_ota_boot.h"
#include <string.h>

// The flag is persisted via the generic bb_nv API, which is a no-op stub on the
// host build (NVS is ESP-only) — so these exercise call-safety of the portable
// surface, not the round-trip (that is covered on-device). Mirrors the
// bb_ota_pull/push host-test pattern.

void test_ota_boot_pending_returns_bool(void)
{
    // Callable without crashing; result is whatever the host stub reports.
    bool p = bb_ota_boot_pending();
    TEST_ASSERT_TRUE(p == true || p == false);
}

void test_ota_boot_arm_callable(void)
{
    bb_ota_boot_arm();
    TEST_ASSERT_TRUE(true);
}

static void noop_progress(bb_ota_phase_t phase, int pct) { (void)phase; (void)pct; }

void test_ota_boot_progress_cb_registration(void)
{
    bb_ota_boot_set_progress_cb(noop_progress);
    bb_ota_boot_set_progress_cb(NULL);
    TEST_ASSERT_TRUE(true);
}

// ---------------------------------------------------------------------------
// bb_ota_boot_phase_str — pure helper, testable on host
// ---------------------------------------------------------------------------

void test_ota_boot_phase_str_start_returns_downloading(void)
{
    TEST_ASSERT_EQUAL_STRING("downloading", bb_ota_boot_phase_str(BB_OTA_PHASE_START));
}

void test_ota_boot_phase_str_progress_returns_downloading(void)
{
    TEST_ASSERT_EQUAL_STRING("downloading", bb_ota_boot_phase_str(BB_OTA_PHASE_PROGRESS));
}

void test_ota_boot_phase_str_success_returns_complete(void)
{
    TEST_ASSERT_EQUAL_STRING("complete", bb_ota_boot_phase_str(BB_OTA_PHASE_SUCCESS));
}

void test_ota_boot_phase_str_fail_returns_error(void)
{
    TEST_ASSERT_EQUAL_STRING("error", bb_ota_boot_phase_str(BB_OTA_PHASE_FAIL));
}

// ---------------------------------------------------------------------------
// bb_ota_boot_set_mdns_service — validation, testable on host
// ---------------------------------------------------------------------------

void test_ota_boot_set_mdns_service_valid(void)
{
    bb_err_t rc = bb_ota_boot_set_mdns_service("my-device", "_myservice", "_tcp", 80);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

void test_ota_boot_set_mdns_service_null_hostname_returns_invalid_arg(void)
{
    bb_err_t rc = bb_ota_boot_set_mdns_service(NULL, "_myservice", "_tcp", 80);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_ota_boot_set_mdns_service_null_service_type_returns_invalid_arg(void)
{
    bb_err_t rc = bb_ota_boot_set_mdns_service("my-device", NULL, "_tcp", 80);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_ota_boot_set_mdns_service_null_proto_returns_invalid_arg(void)
{
    bb_err_t rc = bb_ota_boot_set_mdns_service("my-device", "_myservice", NULL, 80);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_ota_boot_set_mdns_service_hostname_too_long_returns_invalid_arg(void)
{
    // 64 chars — at the OTA_BOOT_MDNS_STR_MAX limit (64), >= check rejects it.
    const char *long_name =
        "a234567890123456789012345678901234567890123456789012345678901234";
    TEST_ASSERT_EQUAL(64, (int)strlen(long_name));
    bb_err_t rc = bb_ota_boot_set_mdns_service(long_name, "_myservice", "_tcp", 80);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_ota_boot_set_mdns_service_hostname_max_minus_one_ok(void)
{
    // 63 chars — one below the limit, should succeed.
    const char *ok_name =
        "a23456789012345678901234567890123456789012345678901234567890123";
    TEST_ASSERT_EQUAL(63, (int)strlen(ok_name));
    bb_err_t rc = bb_ota_boot_set_mdns_service(ok_name, "_svc", "_tcp", 80);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

void test_ota_boot_set_mdns_service_service_type_too_long_returns_invalid_arg(void)
{
    const char *long_svc =
        "a234567890123456789012345678901234567890123456789012345678901234";
    TEST_ASSERT_EQUAL(64, (int)strlen(long_svc));
    bb_err_t rc = bb_ota_boot_set_mdns_service("host", long_svc, "_tcp", 80);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_ota_boot_set_mdns_service_proto_too_long_returns_invalid_arg(void)
{
    const char *long_proto =
        "a234567890123456789012345678901234567890123456789012345678901234";
    TEST_ASSERT_EQUAL(64, (int)strlen(long_proto));
    bb_err_t rc = bb_ota_boot_set_mdns_service("host", "_svc", long_proto, 80);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_ota_boot_set_mdns_service_port_zero_ok(void)
{
    bb_err_t rc = bb_ota_boot_set_mdns_service("host", "_svc", "_tcp", 0);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}
