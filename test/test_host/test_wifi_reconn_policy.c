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
#define HANDSHAKE_REASON BB_WIFI_DISC_HANDSHAKE_TIMEOUT
#define GENERIC_REASON   BB_WIFI_DISC_INACTIVITY

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
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(1, s_state.handshake_fail_count);

    // Disconnect 2
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(2, s_state.handshake_fail_count);

    // Disconnect 3 (tier1 limit)
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
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
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
    }
    TEST_ASSERT_EQUAL(WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT, s_state.handshake_fail_count);

    // 4th disconnect enters tier2
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS, backoff_ms);
    TEST_ASSERT_EQUAL(4, s_state.handshake_fail_count);

    // 6th disconnect still in tier2
    s_state.handshake_fail_count = 5;
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
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
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
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
        action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, GENERIC_REASON, &backoff_ms);
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
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, GENERIC_REASON, &backoff_ms);
    }

    // 11th generic triggers backoff
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, GENERIC_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS, backoff_ms);
    TEST_ASSERT_EQUAL(11, s_state.generic_fail_count);
}

void test_wifi_reconn_5min_escape_hatch(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // First disconnect at t=1sec (from setUp)
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_NOT_EQUAL(WIFI_RECONN_ACTION_REBOOT, action);
    int64_t first_fail_time = s_state.first_fail_us;
    TEST_ASSERT_NOT_EQUAL(0, first_fail_time);

    // Advance past 5 minutes from the first failure
    s_fake_now_us = first_fail_time + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;

    // Second disconnect past window triggers reboot
    action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, GENERIC_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_REBOOT, action);
}

void test_wifi_reconn_got_ip_resets_counters(void)
{
    uint32_t backoff_ms = 0;

    // Build up failures
    for (int i = 0; i < 5; i++) {
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
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
    wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(1, s_state.handshake_fail_count);
}

void test_wifi_reconn_histogram_increments(void)
{
    uint32_t backoff_ms = 0;

    // Disconnect 3 times with reason=BB_WIFI_DISC_NO_AP_FOUND
    for (int i = 0; i < 3; i++) {
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, BB_WIFI_DISC_NO_AP_FOUND, &backoff_ms);
    }

    TEST_ASSERT_EQUAL(3, s_state.reason_histogram[BB_WIFI_DISC_NO_AP_FOUND]);
    TEST_ASSERT_EQUAL(3, s_state.retry_count);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_NO_AP_FOUND, s_state.last_reason);
}

void test_wifi_reconn_state_reset(void)
{
    // Populate state
    s_state.handshake_fail_count = 5;
    s_state.generic_fail_count = 3;
    s_state.first_fail_us = 12345;
    s_state.retry_count = 8;
    s_state.last_reason = BB_WIFI_DISC_BB_LOST_IP;
    s_state.last_disconnect_us = 54321;
    s_state.reason_histogram[BB_WIFI_DISC_AUTH_FAIL] = 2;
    s_state.reason_histogram[BB_WIFI_DISC_BB_LOST_IP] = 7;

    // Reset
    wifi_reconn_state_reset(&s_state);

    TEST_ASSERT_EQUAL(0, s_state.handshake_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.generic_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.first_fail_us);
    TEST_ASSERT_EQUAL(0, s_state.retry_count);
    TEST_ASSERT_EQUAL(0, s_state.last_reason);
    TEST_ASSERT_EQUAL(0, s_state.last_disconnect_us);
    TEST_ASSERT_EQUAL(0, s_state.reason_histogram[BB_WIFI_DISC_AUTH_FAIL]);
    TEST_ASSERT_EQUAL(0, s_state.reason_histogram[BB_WIFI_DISC_BB_LOST_IP]);
}

void test_wifi_reconn_null_args_return_none(void)
{
    uint32_t backoff = 0;
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_disconnect(NULL, &adapter, HANDSHAKE_REASON, &backoff));
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_disconnect(&s_state, NULL, HANDSHAKE_REASON, &backoff));
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_disconnect(&s_state, &adapter, HANDSHAKE_REASON, NULL));

    // Null state for got_ip + reset are no-ops (don't crash)
    wifi_reconn_policy_on_got_ip(NULL);
    wifi_reconn_state_reset(NULL);
}

void test_wifi_reconn_histogram_saturates_at_uint16_max(void)
{
    s_state.reason_histogram[BB_WIFI_DISC_NO_AP_FOUND] = UINT16_MAX;
    uint32_t backoff = 0;
    wifi_reconn_policy_on_disconnect(&s_state, &adapter, BB_WIFI_DISC_NO_AP_FOUND, &backoff);
    TEST_ASSERT_EQUAL(UINT16_MAX, s_state.reason_histogram[BB_WIFI_DISC_NO_AP_FOUND]);
}

void test_wifi_reconn_connect_timeout_null_args_return_none(void)
{
    uint32_t backoff_ms = 0;
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_connect_timeout(NULL, &adapter, &backoff_ms));
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_connect_timeout(&s_state, NULL, &backoff_ms));
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_connect_timeout(&s_state, &adapter, NULL));
}

void test_wifi_reconn_connect_timeout_within_window(void)
{
    uint32_t backoff_ms = 999;
    wifi_reconn_action_t action = wifi_reconn_policy_on_connect_timeout(
        &s_state, &adapter, &backoff_ms);

    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(1, s_state.generic_fail_count);
    TEST_ASSERT_EQUAL(1, s_state.retry_count);
    TEST_ASSERT_NOT_EQUAL(0, s_state.first_fail_us);
    TEST_ASSERT_NOT_EQUAL(0, s_state.last_disconnect_us);
}

void test_wifi_reconn_connect_timeout_fast_retry_limit(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // All GENERIC_FAST_RETRY_LIMIT calls should be RECONNECT_NOW with 0 backoff
    for (int i = 0; i < WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT; i++) {
        action = wifi_reconn_policy_on_connect_timeout(&s_state, &adapter, &backoff_ms);
        TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
        TEST_ASSERT_EQUAL(0, backoff_ms);
    }
    TEST_ASSERT_EQUAL(WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT, s_state.generic_fail_count);
}

void test_wifi_reconn_connect_timeout_backoff(void)
{
    uint32_t backoff_ms = 0;
    wifi_reconn_action_t action;

    // Build up to the fast-retry limit
    for (int i = 0; i < WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT; i++) {
        wifi_reconn_policy_on_connect_timeout(&s_state, &adapter, &backoff_ms);
    }

    // Next timeout beyond the limit triggers progressive backoff
    action = wifi_reconn_policy_on_connect_timeout(&s_state, &adapter, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS, backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT + 1, s_state.generic_fail_count);
}

void test_wifi_reconn_connect_timeout_past_window(void)
{
    uint32_t backoff_ms = 0;

    // First call sets first_fail_us
    wifi_reconn_policy_on_connect_timeout(&s_state, &adapter, &backoff_ms);
    int64_t first_fail = s_state.first_fail_us;
    TEST_ASSERT_NOT_EQUAL(0, first_fail);

    // Advance clock past the 5-min persistent-fail window
    s_fake_now_us = first_fail + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;

    wifi_reconn_action_t action = wifi_reconn_policy_on_connect_timeout(
        &s_state, &adapter, &backoff_ms);

    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_REBOOT, action);
    TEST_ASSERT_EQUAL(2, s_state.generic_fail_count);
    TEST_ASSERT_EQUAL(2, s_state.retry_count);
}

// --- lost-IP policy tests ---

void test_wifi_reconn_should_reconnect_no_ip_associated_no_ip(void)
{
    TEST_ASSERT_TRUE(wifi_reconn_should_reconnect_no_ip(true, false));
}

void test_wifi_reconn_should_reconnect_no_ip_associated_has_ip(void)
{
    TEST_ASSERT_FALSE(wifi_reconn_should_reconnect_no_ip(true, true));
}

void test_wifi_reconn_should_reconnect_no_ip_not_associated(void)
{
    TEST_ASSERT_FALSE(wifi_reconn_should_reconnect_no_ip(false, false));
}

void test_wifi_reconn_policy_on_lost_ip_increments(void)
{
    // Reset and use the module-level s_state and adapter
    TEST_ASSERT_EQUAL(0, s_state.lost_ip_count);
    TEST_ASSERT_EQUAL(0, s_state.last_lost_ip_us);
    TEST_ASSERT_EQUAL(0, s_state.reason_histogram[WIFI_REASON_BB_LOST_IP]);

    s_fake_now_us = 5000000;
    wifi_reconn_policy_on_lost_ip(&s_state, &adapter);

    TEST_ASSERT_EQUAL(1, s_state.lost_ip_count);
    TEST_ASSERT_EQUAL_INT64(5000000, s_state.last_lost_ip_us);
    TEST_ASSERT_EQUAL(1, s_state.reason_histogram[WIFI_REASON_BB_LOST_IP]);

    // Second call
    s_fake_now_us = 10000000;
    wifi_reconn_policy_on_lost_ip(&s_state, &adapter);
    TEST_ASSERT_EQUAL(2, s_state.lost_ip_count);
    TEST_ASSERT_EQUAL_INT64(10000000, s_state.last_lost_ip_us);
    TEST_ASSERT_EQUAL(2, s_state.reason_histogram[WIFI_REASON_BB_LOST_IP]);
}

void test_wifi_reconn_policy_on_lost_ip_null_args(void)
{
    // No crash on null args
    wifi_reconn_policy_on_lost_ip(NULL, &adapter);
    wifi_reconn_policy_on_lost_ip(&s_state, NULL);
}

void test_wifi_reconn_policy_on_lost_ip_histogram_saturates(void)
{
    s_state.reason_histogram[WIFI_REASON_BB_LOST_IP] = UINT16_MAX;
    wifi_reconn_policy_on_lost_ip(&s_state, &adapter);
    TEST_ASSERT_EQUAL(UINT16_MAX, s_state.reason_histogram[WIFI_REASON_BB_LOST_IP]);
}

void test_wifi_reconn_policy_arms_first_fail_on_inactivity_disconnect(void)
{
    // Simulates an inactivity (beacon timeout) DISCONNECT flowing into the
    // policy (generic disconnect path, same as inactive-time triggered
    // disconnect).
    uint32_t backoff_ms = 0;
    TEST_ASSERT_EQUAL(0, s_state.first_fail_us);

    wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(
        &s_state, &adapter, BB_WIFI_DISC_INACTIVITY, &backoff_ms);

    // First disconnect: arms first_fail_us; no reboot yet.
    TEST_ASSERT_NOT_EQUAL(0, s_state.first_fail_us);
    TEST_ASSERT_NOT_EQUAL(WIFI_RECONN_ACTION_REBOOT, action);
    TEST_ASSERT_EQUAL(1, s_state.generic_fail_count);
}

// --- egress probe policy tests ---

void test_wifi_reconn_on_egress_probe_reachable_resets_streak(void)
{
    // Pre-load a partial streak
    s_state.egress_fail_streak = 2;
    wifi_reconn_action_t action = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/true, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE, action);
    TEST_ASSERT_EQUAL(0, s_state.egress_fail_streak);
    TEST_ASSERT_EQUAL(0, s_state.egress_dead_count);
}

void test_wifi_reconn_on_egress_probe_streak_below_threshold(void)
{
    // Two failures below threshold of 3 → NONE
    wifi_reconn_action_t a1 = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE, a1);
    TEST_ASSERT_EQUAL(1, s_state.egress_fail_streak);
    TEST_ASSERT_EQUAL(0, s_state.egress_dead_count);

    wifi_reconn_action_t a2 = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE, a2);
    TEST_ASSERT_EQUAL(2, s_state.egress_fail_streak);
    TEST_ASSERT_EQUAL(0, s_state.egress_dead_count);
}

void test_wifi_reconn_on_egress_probe_at_threshold_returns_recover(void)
{
    s_fake_now_us = 7000000;

    // Two below-threshold failures
    wifi_reconn_policy_on_egress_probe(&s_state, &adapter, false, 3);
    wifi_reconn_policy_on_egress_probe(&s_state, &adapter, false, 3);

    // Third (= threshold) triggers recovery
    wifi_reconn_action_t action = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);

    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(1, s_state.egress_dead_count);
    TEST_ASSERT_EQUAL(0, s_state.egress_fail_streak);   // reset after threshold
    TEST_ASSERT_EQUAL_INT64(7000000, s_state.last_egress_dead_us);
    TEST_ASSERT_NOT_EQUAL(0, s_state.first_fail_us);
    TEST_ASSERT_EQUAL(1, s_state.reason_histogram[WIFI_REASON_BB_EGRESS_DEAD]);
}

void test_wifi_reconn_on_egress_probe_null_args_safe(void)
{
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_egress_probe(NULL, &adapter, false, 3));
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE,
        wifi_reconn_policy_on_egress_probe(&s_state, NULL, false, 3));
}

void test_wifi_reconn_on_egress_probe_histogram_saturates(void)
{
    // Set histogram to saturated (UINT16_MAX)
    s_state.reason_histogram[WIFI_REASON_BB_EGRESS_DEAD] = UINT16_MAX;

    // Drive THRESH-1 failures to reach just below threshold (streak = 2, threshold = 3)
    wifi_reconn_action_t a1 = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE, a1);
    TEST_ASSERT_EQUAL(1, s_state.egress_fail_streak);

    wifi_reconn_action_t a2 = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE, a2);
    TEST_ASSERT_EQUAL(2, s_state.egress_fail_streak);

    // Third (= threshold) triggers recovery
    wifi_reconn_action_t a3 = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, a3);

    // Verify histogram stayed UINT16_MAX (line 160 false branch coverage)
    TEST_ASSERT_EQUAL(UINT16_MAX, s_state.reason_histogram[WIFI_REASON_BB_EGRESS_DEAD]);
}

// --- no-IP watchdog policy tests (B1-381) ---

void test_wifi_reconn_policy_on_no_ip_increments(void)
{
    TEST_ASSERT_EQUAL(0, s_state.no_ip_count);
    TEST_ASSERT_EQUAL(0, s_state.last_no_ip_us);
    TEST_ASSERT_EQUAL(0, s_state.reason_histogram[WIFI_REASON_BB_NO_IP_WATCHDOG]);

    s_fake_now_us = 3000000;
    wifi_reconn_policy_on_no_ip(&s_state, &adapter);

    TEST_ASSERT_EQUAL(1, s_state.no_ip_count);
    TEST_ASSERT_EQUAL_INT64(3000000, s_state.last_no_ip_us);
    TEST_ASSERT_EQUAL(1, s_state.reason_histogram[WIFI_REASON_BB_NO_IP_WATCHDOG]);

    s_fake_now_us = 6000000;
    wifi_reconn_policy_on_no_ip(&s_state, &adapter);
    TEST_ASSERT_EQUAL(2, s_state.no_ip_count);
    TEST_ASSERT_EQUAL_INT64(6000000, s_state.last_no_ip_us);
    TEST_ASSERT_EQUAL(2, s_state.reason_histogram[WIFI_REASON_BB_NO_IP_WATCHDOG]);
}

void test_wifi_reconn_policy_on_no_ip_null_args(void)
{
    // No crash on null args
    wifi_reconn_policy_on_no_ip(NULL, &adapter);
    wifi_reconn_policy_on_no_ip(&s_state, NULL);
}

void test_wifi_reconn_policy_on_no_ip_arms_first_fail(void)
{
    TEST_ASSERT_EQUAL(0, s_state.first_fail_us);
    s_fake_now_us = 4000000;
    wifi_reconn_policy_on_no_ip(&s_state, &adapter);
    // first_fail_us should be armed
    TEST_ASSERT_EQUAL_INT64(4000000, s_state.first_fail_us);
}

void test_wifi_reconn_policy_on_no_ip_first_fail_already_armed(void)
{
    s_state.first_fail_us = 99999;
    s_fake_now_us = 5000000;
    wifi_reconn_policy_on_no_ip(&s_state, &adapter);
    // first_fail_us must NOT change if already set
    TEST_ASSERT_EQUAL_INT64(99999, s_state.first_fail_us);
}

void test_wifi_reconn_policy_on_no_ip_histogram_saturates(void)
{
    s_state.reason_histogram[WIFI_REASON_BB_NO_IP_WATCHDOG] = UINT16_MAX;
    wifi_reconn_policy_on_no_ip(&s_state, &adapter);
    TEST_ASSERT_EQUAL(UINT16_MAX, s_state.reason_histogram[WIFI_REASON_BB_NO_IP_WATCHDOG]);
}

void test_wifi_reconn_on_egress_probe_first_fail_already_armed(void)
{
    // Pre-arm first_fail_us with a known value
    s_state.first_fail_us = 12345;

    // Drive THRESH-1 failures to reach just below threshold
    wifi_reconn_action_t a1 = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE, a1);
    TEST_ASSERT_EQUAL(1, s_state.egress_fail_streak);

    wifi_reconn_action_t a2 = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_NONE, a2);
    TEST_ASSERT_EQUAL(2, s_state.egress_fail_streak);

    // Third (= threshold) triggers recovery
    wifi_reconn_action_t a3 = wifi_reconn_policy_on_egress_probe(
        &s_state, &adapter, /*reachable=*/false, 3);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, a3);

    // Verify first_fail_us is UNCHANGED (line 163 false branch coverage)
    TEST_ASSERT_EQUAL_INT64(12345, s_state.first_fail_us);
}
