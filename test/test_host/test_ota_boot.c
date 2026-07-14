#include "unity.h"
#include "bb_ota_boot.h"
#include "bb_config.h"
#include "bb_storage.h"
#include "fake_nvs_backend.h"
#include <string.h>

// B1-756 (bb_nv dissolution epic B1-708): the one-shot boot-mode flag now
// round-trips through bb_config (backend="nvs") instead of bb_nv's generic
// KV forwarder. Previously bb_nv_get_u8/set_u8 were genuine no-op stubs on
// host (integers never round-tripped there, only strings did) so these
// call-safety tests never actually exercised persistence; the new test below
// registers fake_nvs_backend.h's fake "nvs" backend and proves a real
// round-trip, which the host build can now genuinely exercise. Mirrors the
// bb_ota_pull/push host-test pattern for the remaining call-safety checks.

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

// Test-local field descriptor targeting the EXACT SAME address (backend/ns/
// key/type) as bb_ota_boot.c's internal s_ota_boot_flag_field -- a
// literal-address BITE test proving bb_ota_boot_arm/bb_ota_boot_pending
// actually land on/read from that address, not merely that they're callable.
static const bb_config_field_t s_test_ota_boot_flag_field = {
    .id   = "test.ota_boot.flag", .type = BB_CONFIG_U8,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "ota_boot_mode" },
};

void test_ota_boot_arm_then_pending_round_trips_through_storage(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL));

    TEST_ASSERT_FALSE(bb_ota_boot_pending());  // unset -> default 0 -> false

    bb_ota_boot_arm();
    TEST_ASSERT_TRUE(bb_ota_boot_pending());

    // BITE: the literal "bb_cfg"/"ota_boot_mode" address (the SAME one
    // bb_nv used) must hold the armed value.
    uint8_t flag_out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&s_test_ota_boot_flag_field, &flag_out));
    TEST_ASSERT_EQUAL_UINT8(1, flag_out);

    bb_storage_test_reset();
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
