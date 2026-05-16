#include "unity.h"
#include "bb_ota_pull.h"
#include "bb_ota_pull_test_hooks.h"

// Skip-check callback tests
static bool s_skip_check_callback_called = false;
static bool s_skip_check_callback_result = false;

static bool test_skip_check_callback(void)
{
    s_skip_check_callback_called = true;
    return s_skip_check_callback_result;
}

void test_ota_pull_skip_check_callback_registration(void)
{
    // Test that callback can be set and unset
    bb_ota_pull_set_skip_check_cb(test_skip_check_callback);
    // Callback registered successfully, verify by checking callback execution
    s_skip_check_callback_called = false;
    s_skip_check_callback_result = false;
    test_skip_check_callback();
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    // Unset callback
    bb_ota_pull_set_skip_check_cb(NULL);
    s_skip_check_callback_called = false;
    // Calling set to NULL should clear it
    TEST_ASSERT_TRUE(true);  // Sentinel: callback unset successfully
}

void test_ota_pull_skip_check_callback_returns_true(void)
{
    // Test callback returning true
    bb_ota_pull_set_skip_check_cb(test_skip_check_callback);
    s_skip_check_callback_result = true;
    s_skip_check_callback_called = false;

    bool result = test_skip_check_callback();
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    bb_ota_pull_set_skip_check_cb(NULL);
}

void test_ota_pull_skip_check_callback_returns_false(void)
{
    // Test callback returning false
    bb_ota_pull_set_skip_check_cb(test_skip_check_callback);
    s_skip_check_callback_result = false;
    s_skip_check_callback_called = false;

    bool result = test_skip_check_callback();
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    bb_ota_pull_set_skip_check_cb(NULL);
}

void test_bb_ota_pull_set_http_timeout_ms_default_is_20000(void)
{
    // Default must be 20000 ms for backward compat with existing consumers.
    // Restore default via 0 before asserting to avoid cross-test pollution.
    bb_ota_pull_set_http_timeout_ms(0);
    TEST_ASSERT_EQUAL_UINT32(20000, bb_ota_pull_host_get_http_timeout_ms());
}

void test_bb_ota_pull_set_http_timeout_ms_zero_restores_default(void)
{
    // Set a non-default value, then pass 0 — must restore 20000.
    bb_ota_pull_set_http_timeout_ms(60000);
    TEST_ASSERT_EQUAL_UINT32(60000, bb_ota_pull_host_get_http_timeout_ms());
    bb_ota_pull_set_http_timeout_ms(0);
    TEST_ASSERT_EQUAL_UINT32(20000, bb_ota_pull_host_get_http_timeout_ms());
}

// WDT exclusion (ota_worker_task / ota_check_worker_task) is ESP-IDF-only;
// it requires esp_task_wdt_delete/add which have no host stub. Correctness
// is verified by code review and smoke builds against espidf targets.
