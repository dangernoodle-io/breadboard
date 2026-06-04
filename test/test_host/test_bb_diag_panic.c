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
