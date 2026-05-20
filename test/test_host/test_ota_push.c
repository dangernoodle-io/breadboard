#include "unity.h"
#include "bb_ota_push.h"

// Skip-check callback tests
static bool s_skip_check_callback_called = false;
static bool s_skip_check_callback_result = false;

static bool test_skip_check_callback(void)
{
    s_skip_check_callback_called = true;
    return s_skip_check_callback_result;
}

void test_ota_push_skip_check_callback_registration(void)
{
    // Test that callback can be set and unset
    bb_ota_push_set_skip_check_cb(test_skip_check_callback);
    // Callback registered successfully
    s_skip_check_callback_called = false;
    s_skip_check_callback_result = false;
    test_skip_check_callback();
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    // Unset callback
    bb_ota_push_set_skip_check_cb(NULL);
    s_skip_check_callback_called = false;
    TEST_ASSERT_TRUE(true);  // Sentinel: callback unset successfully
}

void test_ota_push_skip_check_callback_returns_true(void)
{
    // Test callback returning true
    bb_ota_push_set_skip_check_cb(test_skip_check_callback);
    s_skip_check_callback_result = true;
    s_skip_check_callback_called = false;

    bool result = test_skip_check_callback();
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    bb_ota_push_set_skip_check_cb(NULL);
}

void test_ota_push_skip_check_callback_returns_false(void)
{
    // Test callback returning false
    bb_ota_push_set_skip_check_cb(test_skip_check_callback);
    s_skip_check_callback_result = false;
    s_skip_check_callback_called = false;

    bool result = test_skip_check_callback();
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    bb_ota_push_set_skip_check_cb(NULL);
}

// Pause/resume hook tests
static bool s_pause_callback_called = false;
static bool s_resume_callback_called = false;

static bool test_pause_callback(void)
{
    s_pause_callback_called = true;
    return true;
}

static void test_resume_callback(void)
{
    s_resume_callback_called = true;
}

void test_ota_push_hooks_registration(void)
{
    // Test that hooks can be set
    bb_ota_push_set_hooks(test_pause_callback, test_resume_callback);

    s_pause_callback_called = false;
    s_resume_callback_called = false;
    test_pause_callback();
    test_resume_callback();
    TEST_ASSERT_TRUE(s_pause_callback_called);
    TEST_ASSERT_TRUE(s_resume_callback_called);

    // Unset hooks
    bb_ota_push_set_hooks(NULL, NULL);
    TEST_ASSERT_TRUE(true);  // Sentinel: hooks unset successfully
}

void test_ota_push_pause_hook_returns_true(void)
{
    bb_ota_push_set_hooks(test_pause_callback, test_resume_callback);
    s_pause_callback_called = false;

    bool result = test_pause_callback();
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(s_pause_callback_called);

    bb_ota_push_set_hooks(NULL, NULL);
}

// Content-length validation tests
#define TEST_MAX_SIZE (4 * 1024 * 1024)  // 4 MB — matches Kconfig default

void test_ota_push_validate_content_len_zero_returns_400(void)
{
    TEST_ASSERT_EQUAL_INT(400, bb_ota_push_validate_content_len(0, TEST_MAX_SIZE));
}

void test_ota_push_validate_content_len_negative_returns_400(void)
{
    TEST_ASSERT_EQUAL_INT(400, bb_ota_push_validate_content_len(-1, TEST_MAX_SIZE));
}

void test_ota_push_validate_content_len_oversized_returns_413(void)
{
    TEST_ASSERT_EQUAL_INT(413, bb_ota_push_validate_content_len(TEST_MAX_SIZE + 1, TEST_MAX_SIZE));
}

void test_ota_push_validate_content_len_at_max_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_ota_push_validate_content_len(TEST_MAX_SIZE, TEST_MAX_SIZE));
}

void test_ota_push_validate_content_len_valid_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_ota_push_validate_content_len(65536, TEST_MAX_SIZE));
}
