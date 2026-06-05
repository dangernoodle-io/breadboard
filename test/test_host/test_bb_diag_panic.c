#include "unity.h"
#include "bb_diag.h"
#include "bb_core.h"

void test_bb_diag_panic_available_returns_false_on_host(void)
{
    // Host implementation always returns false
    TEST_ASSERT_FALSE(bb_diag_panic_available());
}

void test_bb_diag_panic_get_returns_not_found_on_host(void)
{
    char buf[256];
    size_t len = sizeof(buf);
    bb_err_t err = bb_diag_panic_get(buf, &len);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

void test_bb_diag_panic_get_invalid_args(void)
{
    char buf[256];
    size_t len = 0;
    // Zero-length buffer is invalid
    bb_err_t err = bb_diag_panic_get(buf, &len);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    // NULL pointer is invalid (on host this won't crash, but still NOT_FOUND)
    len = sizeof(buf);
    err = bb_diag_panic_get(NULL, &len);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    // NULL len_inout is invalid
    err = bb_diag_panic_get(buf, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

void test_bb_diag_panic_clear_is_safe_on_host(void)
{
    // Should be a safe no-op on host
    bb_diag_panic_clear();
    TEST_PASS();
}

void test_bb_diag_panic_clear_after_unavailable(void)
{
    // Even if panic isn't available, clear should be safe
    TEST_ASSERT_FALSE(bb_diag_panic_available());
    bb_diag_panic_clear();
    TEST_ASSERT_FALSE(bb_diag_panic_available());
}

void test_bb_diag_abnormal_reset_count_returns_zero_on_host(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_diag_abnormal_reset_count());
}

void test_bb_diag_abnormal_reset_count_clear_is_safe_on_host(void)
{
    // Should be a safe no-op on host
    bb_diag_abnormal_reset_count_clear();
    TEST_ASSERT_EQUAL_UINT32(0, bb_diag_abnormal_reset_count());
}

void test_bb_diag_panic_app_sha_returns_not_found_on_host(void)
{
    char buf[BB_DIAG_PANIC_APP_SHA256_MAX];
    bb_err_t err = bb_diag_panic_app_sha(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

void test_bb_diag_panic_app_sha_invalid_args(void)
{
    char buf[BB_DIAG_PANIC_APP_SHA256_MAX];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_panic_app_sha(NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_panic_app_sha(buf, 0));
}

void test_bb_diag_panic_coredump_erase_is_safe_on_host(void)
{
    // no-op on host — must not crash
    bb_diag_panic_coredump_erase();
    TEST_PASS();
}

void test_bb_diag_panic_coredump_erase_idempotent_on_host(void)
{
    // calling twice must also be safe
    bb_diag_panic_coredump_erase();
    bb_diag_panic_coredump_erase();
    TEST_PASS();
}

// ---- bb_diag_reset_decision pure-function tests ----

// (a) First boot: stored_fp == 0 → count reset to 0, fp must be stored
void test_bb_diag_reset_decision_first_boot_clean(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0, 0xDEADBEEF, 5, false);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_TRUE(r.store_fp);
}

void test_bb_diag_reset_decision_first_boot_abnormal(void)
{
    // Deploy boot is the clean baseline — NOT counted even if reset reason is abnormal
    bb_diag_reset_result_t r = bb_diag_reset_decision(0, 0xDEADBEEF, 3, true);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_TRUE(r.store_fp);
}

// (b) New firmware (stored_fp != running_fp) + abnormal → counter RESETS to 0, new fp stored
void test_bb_diag_reset_decision_new_firmware_abnormal(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0xAAAAAAAA, 0xBBBBBBBB, 7, true);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_TRUE(r.store_fp);
}

// New firmware, clean reset → also resets to 0 and stores fp
void test_bb_diag_reset_decision_new_firmware_clean(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0x11111111, 0x22222222, 10, false);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_TRUE(r.store_fp);
}

// (c) Same firmware + abnormal → count increments by 1
void test_bb_diag_reset_decision_same_firmware_abnormal(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0xCAFEBABE, 0xCAFEBABE, 4, true);
    TEST_ASSERT_EQUAL_UINT32(5, r.new_count);
    TEST_ASSERT_FALSE(r.store_fp);
}

// (d) Same firmware + clean reset → count unchanged
void test_bb_diag_reset_decision_same_firmware_clean(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0x12345678, 0x12345678, 3, false);
    TEST_ASSERT_EQUAL_UINT32(3, r.new_count);
    TEST_ASSERT_FALSE(r.store_fp);
}

// Same firmware, count starts at 0, clean → stays 0
void test_bb_diag_reset_decision_same_firmware_clean_from_zero(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0xABCDABCD, 0xABCDABCD, 0, false);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_FALSE(r.store_fp);
}
