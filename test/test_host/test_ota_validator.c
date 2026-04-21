#include "unity.h"
#include "bb_ota_validator.h"

// setUp and tearDown are defined in test_main.c; this file just defines tests.

// On the host backend, bb_ota_is_pending() always returns false (no OTA partition).
void test_ota_validator_is_pending_false_on_host(void)
{
    TEST_ASSERT_FALSE(bb_ota_is_pending());
}

// bb_ota_mark_valid returns BB_ERR_INVALID_STATE on host (not pending).
void test_ota_validator_mark_valid_returns_invalid_state_on_host(void)
{
    bb_err_t rc = bb_ota_mark_valid("test-reason");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

// bb_ota_mark_valid with NULL reason also returns BB_ERR_INVALID_STATE on host.
void test_ota_validator_mark_valid_null_reason_on_host(void)
{
    bb_err_t rc = bb_ota_mark_valid(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

// Multiple calls to mark_valid all return BB_ERR_INVALID_STATE on host.
void test_ota_validator_mark_valid_idempotent_on_host(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_mark_valid("first"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_mark_valid("second"));
}
