#include "unity.h"
#include "wifi_reconn_policy.h"

// Fake time for testing
static int64_t s_fake_now_us = 0;

static int64_t fake_now(void)
{
    return s_fake_now_us;
}

static const wifi_reconn_adapter_t adapter = {
    .now_us = fake_now,
};

static wifi_reconn_state_t s_state;

// Helper constants for testing
#define HANDSHAKE_REASON 15
#define GENERIC_REASON   200

// Reset hook called from test_main.c's setUp
void wifi_reconn_policy_test_reset(void)
{
    s_fake_now_us = 1000000;  // Start at 1 second to avoid t=0 edge cases
    wifi_reconn_state_reset(&s_state);
}

void test_wifi_reconn_tier1_handshake_fast_retry(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // Disconnect 1
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                               HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(1, s_state.handshake_fail_count);

    // Disconnect 2
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                               HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(2, s_state.handshake_fail_count);

    // Disconnect 3 (tier1 limit)
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                               HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(3, s_state.handshake_fail_count);
}

void test_wifi_reconn_tier2_handshake_backoff(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // Build up to tier2
    for (int i = 0; i < WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT; i++) {
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                         HANDSHAKE_REASON, &backoff_ms);
    }
    TEST_ASSERT_EQUAL(WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT, s_state.handshake_fail_count);

    // 4th disconnect enters tier2
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                               HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS, backoff_ms);
    TEST_ASSERT_EQUAL(4, s_state.handshake_fail_count);

    // 6th disconnect still in tier2
    s_state.handshake_fail_count = 5;
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                               HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS, backoff_ms);
    TEST_ASSERT_EQUAL(6, s_state.handshake_fail_count);
}

void test_wifi_reconn_tier3_handshake_backoff(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // Jump to tier3 threshold
    s_state.handshake_fail_count = WIFI_RECONN_HANDSHAKE_TIER2_LIMIT;

    // 7th disconnect enters tier3
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                               HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS, backoff_ms);
    TEST_ASSERT_EQUAL(7, s_state.handshake_fail_count);
}

void test_wifi_reconn_generic_fast_retry(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // 10 generic fast retries
    for (int i = 0; i < WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT; i++) {
        action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, GENERIC_REASON, HANDSHAKE_REASON,
                                                   &backoff_ms);
        TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
        TEST_ASSERT_EQUAL(0, backoff_ms);
    }
    TEST_ASSERT_EQUAL(WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT, s_state.generic_fail_count);
}

void test_wifi_reconn_generic_backoff(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // Build up to the limit
    for (int i = 0; i < WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT; i++) {
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, GENERIC_REASON, HANDSHAKE_REASON,
                                         &backoff_ms);
    }

    // 11th generic triggers backoff
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, GENERIC_REASON, HANDSHAKE_REASON,
                                               &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS, backoff_ms);
    TEST_ASSERT_EQUAL(11, s_state.generic_fail_count);
}

void test_wifi_reconn_5min_escape_hatch(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // First disconnect at t=1sec (from setUp)
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                               HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_NOT_EQUAL(WIFI_RECONN_ACTION_REBOOT, action);
    int64_t first_fail_time = s_state.first_fail_us;
    TEST_ASSERT_NOT_EQUAL(0, first_fail_time);

    // Advance past 5 minutes from the first failure
    s_fake_now_us = first_fail_time + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;

    // Second disconnect past window triggers reboot
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, GENERIC_REASON,
                                               HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_REBOOT, action);
}

void test_wifi_reconn_got_ip_resets_counters(void)
{
    uint32_t backoff_ms = 0;

    // Build up failures
    for (int i = 0; i < 5; i++) {
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                         HANDSHAKE_REASON, &backoff_ms);
    }
    TEST_ASSERT_EQUAL(5, s_state.handshake_fail_count);
    TEST_ASSERT_NOT_EQUAL(0, s_state.first_fail_us);
    TEST_ASSERT_EQUAL(5, s_state.retry_count);

    // Got IP resets everything
    wifi_reconn_policy_on_got_ip(&s_state);
    TEST_ASSERT_EQUAL(0, s_state.handshake_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.generic_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.first_fail_us);
    TEST_ASSERT_EQUAL(0, s_state.retry_count);

    // Next disconnect is back to tier1
    wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON,
                                                                     HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(1, s_state.handshake_fail_count);
}

void test_wifi_reconn_histogram_increments(void)
{
    uint32_t backoff_ms = 0;

    // Disconnect 3 times with reason=42
    for (int i = 0; i < 3; i++) {
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, 42, HANDSHAKE_REASON,
                                         &backoff_ms);
    }

    TEST_ASSERT_EQUAL(3, s_state.reason_histogram[42]);
    TEST_ASSERT_EQUAL(3, s_state.retry_count);
    TEST_ASSERT_EQUAL(42, s_state.last_reason);
}

void test_wifi_reconn_state_reset(void)
{
    // Populate state
    s_state.handshake_fail_count = 5;
    s_state.generic_fail_count = 3;
    s_state.first_fail_us = 12345;
    s_state.retry_count = 8;
    s_state.last_reason = 100;
    s_state.last_disconnect_us = 54321;
    s_state.reason_histogram[50] = 2;
    s_state.reason_histogram[100] = 7;

    // Reset
    wifi_reconn_state_reset(&s_state);

    TEST_ASSERT_EQUAL(0, s_state.handshake_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.generic_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.first_fail_us);
    TEST_ASSERT_EQUAL(0, s_state.retry_count);
    TEST_ASSERT_EQUAL(0, s_state.last_reason);
    TEST_ASSERT_EQUAL(0, s_state.last_disconnect_us);
    TEST_ASSERT_EQUAL(0, s_state.reason_histogram[50]);
    TEST_ASSERT_EQUAL(0, s_state.reason_histogram[100]);
}

void test_wifi_reconn_null_args_return_none(void)
{
    uint32_t backoff = 0;
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_disconnect(NULL, &adapter, HANDSHAKE_REASON, HANDSHAKE_REASON, &backoff));
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_disconnect(&s_state, NULL, HANDSHAKE_REASON, HANDSHAKE_REASON, &backoff));
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, HANDSHAKE_REASON, NULL));

    // Null state for got_ip + reset are no-ops (don't crash)
    wifi_reconn_policy_on_got_ip(NULL);
    wifi_reconn_state_reset(NULL);
}

void test_wifi_reconn_histogram_saturates_at_uint16_max(void)
{
    s_state.reason_histogram[42] = UINT16_MAX;
    uint32_t backoff = 0;
    wifi_reconn_policy_on_disconnect(&s_state, &adapter, 42, HANDSHAKE_REASON, &backoff);
    TEST_ASSERT_EQUAL(UINT16_MAX, s_state.reason_histogram[42]);
}
