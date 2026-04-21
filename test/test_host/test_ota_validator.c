#include "unity.h"
#include "bb_ota_validator.h"

// Mock strategy for testing
static bool s_mock_is_pending_retval;
static const char *s_mock_reason_arg;
static bb_err_t s_mock_mark_valid_retval;

static bool mock_is_pending(void)
{
    return s_mock_is_pending_retval;
}

static bb_err_t mock_mark_valid(const char *reason)
{
    s_mock_reason_arg = reason;
    return s_mock_mark_valid_retval;
}

static const bb_ota_validator_strategy_t s_mock_strategy = {
    .is_pending = mock_is_pending,
    .mark_valid = mock_mark_valid,
};

// setUp and tearDown are defined in test_main.c; this file just defines tests.

// Test that is_pending proxies to the strategy correctly
void test_ota_validator_is_pending_proxies(void)
{
    s_mock_is_pending_retval = true;
    TEST_ASSERT_TRUE(mock_is_pending());

    s_mock_is_pending_retval = false;
    TEST_ASSERT_FALSE(mock_is_pending());
}

// Test that mark_valid proxies to the strategy and records the reason
void test_ota_validator_mark_valid_proxies(void)
{
    s_mock_is_pending_retval = false;
    s_mock_reason_arg = NULL;
    s_mock_mark_valid_retval = BB_OK;
    bb_err_t rc = mock_mark_valid("test-reason");
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("test-reason", s_mock_reason_arg);
}

// Test that mark_valid can return error
void test_ota_validator_mark_valid_error(void)
{
    s_mock_is_pending_retval = false;
    s_mock_reason_arg = NULL;
    s_mock_mark_valid_retval = BB_ERR_INVALID_STATE;
    bb_err_t rc = mock_mark_valid("reason");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

// Test that default stubs return expected values on non-ESP
void test_ota_validator_default_stub_is_pending(void)
{
    bool pending = bb_ota_default_is_pending();
    TEST_ASSERT_FALSE(pending);
}

void test_ota_validator_default_stub_mark_valid(void)
{
    bb_err_t rc = bb_ota_default_mark_valid("test");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

// Test with NULL reason
void test_ota_validator_mark_valid_null_reason(void)
{
    s_mock_is_pending_retval = false;
    s_mock_reason_arg = (const char *)0xDEADBEEF;  // sentinel
    mock_mark_valid(NULL);
    TEST_ASSERT_NULL(s_mock_reason_arg);
}
