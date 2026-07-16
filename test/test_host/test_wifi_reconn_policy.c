#include "unity.h"
#include "wifi_reconn_policy.h"

#include <string.h>

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
    s_state.slow_fail_count = 2;
    wifi_reconn_policy_on_got_ip(&s_state);
    TEST_ASSERT_EQUAL(0, s_state.handshake_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.generic_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.slow_fail_count);
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
    s_state.slow_fail_count = 4;
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
    TEST_ASSERT_EQUAL(0, s_state.slow_fail_count);
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

// --- SLOW backoff tier tests (PR7, B1-994/B1-806) ---

void test_wifi_reconn_slow_tier_no_hot_retry(void)
{
    uint32_t backoff_ms = 999;
    wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(
        &s_state, &adapter, BB_WIFI_DISC_AUTH_FAIL, &backoff_ms);

    // Even the FIRST AUTH_FAIL disconnect backs off -- never a hot retry.
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_SLOW_BACKOFF_TIER1_MS, backoff_ms);
    TEST_ASSERT_EQUAL(1, s_state.slow_fail_count);
}

void test_wifi_reconn_slow_tier2_escalation(void)
{
    uint32_t backoff_ms = 0;

    for (int i = 0; i < WIFI_RECONN_SLOW_TIER1_LIMIT; i++) {
        wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(
            &s_state, &adapter, BB_WIFI_DISC_NO_AP_FOUND, &backoff_ms);
        TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
        TEST_ASSERT_EQUAL(WIFI_RECONN_SLOW_BACKOFF_TIER1_MS, backoff_ms);
    }
    TEST_ASSERT_EQUAL(WIFI_RECONN_SLOW_TIER1_LIMIT, s_state.slow_fail_count);

    // Beyond the tier-1 limit, escalate to tier-2.
    wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(
        &s_state, &adapter, BB_WIFI_DISC_NO_AP_FOUND, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_SCHEDULE_BACKOFF, action);
    TEST_ASSERT_EQUAL(WIFI_RECONN_SLOW_BACKOFF_TIER2_MS, backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_SLOW_TIER1_LIMIT + 1, s_state.slow_fail_count);
}

void test_wifi_reconn_slow_tier_shared_by_auth_and_no_ap_found(void)
{
    uint32_t backoff_ms = 0;

    // AUTH_FAIL and NO_AP_FOUND advance the SAME slow_fail_count counter.
    wifi_reconn_policy_on_disconnect(&s_state, &adapter, BB_WIFI_DISC_AUTH_FAIL, &backoff_ms);
    TEST_ASSERT_EQUAL(1, s_state.slow_fail_count);

    wifi_reconn_policy_on_disconnect(&s_state, &adapter, BB_WIFI_DISC_NO_AP_FOUND, &backoff_ms);
    TEST_ASSERT_EQUAL(2, s_state.slow_fail_count);

    wifi_reconn_policy_on_disconnect(&s_state, &adapter, BB_WIFI_DISC_AUTH_FAIL, &backoff_ms);
    TEST_ASSERT_EQUAL(3, s_state.slow_fail_count);
}

void test_wifi_reconn_handshake_tier_unaffected_by_slow_tier(void)
{
    uint32_t backoff_ms = 0;

    // Prime the slow counter, then confirm the handshake ladder is untouched.
    wifi_reconn_policy_on_disconnect(&s_state, &adapter, BB_WIFI_DISC_AUTH_FAIL, &backoff_ms);
    wifi_reconn_policy_on_disconnect(&s_state, &adapter, BB_WIFI_DISC_NO_AP_FOUND, &backoff_ms);
    TEST_ASSERT_EQUAL(2, s_state.slow_fail_count);
    TEST_ASSERT_EQUAL(0, s_state.handshake_fail_count);

    wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(
        &s_state, &adapter, HANDSHAKE_REASON, &backoff_ms);
    TEST_ASSERT_EQUAL(WIFI_RECONN_ACTION_RECONNECT_NOW, action);
    TEST_ASSERT_EQUAL(0, backoff_ms);
    TEST_ASSERT_EQUAL(1, s_state.handshake_fail_count);
    TEST_ASSERT_EQUAL(2, s_state.slow_fail_count);
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

// Review fix [MEDIUM] regression guard (B1-805 slice 1a): the device shell
// (platform/espidf/bb_wifi/wifi_reconn.c) records lost-IP diagnostics into a
// DEDICATED event-task-owned instance (s_lost_ip_diag), never the reconn-
// task-owned FSM ctx's policy state (single-writer contract on
// wifi_reconn_ctx_t) -- verifies wifi_reconn_policy_on_lost_ip only mutates
// the instance it's handed, so wifi_reconn_get_lost_ip_count()/_age_us()
// (which read exactly this instance's fields) stay accurate without ever
// touching a separate FSM ctx's policy.
void test_wifi_reconn_policy_on_lost_ip_isolated_from_other_instances(void)
{
    wifi_reconn_state_t other_ctx_policy;
    wifi_reconn_state_reset(&other_ctx_policy);

    s_fake_now_us = 5000000;
    wifi_reconn_policy_on_lost_ip(&s_state, &adapter);

    TEST_ASSERT_EQUAL(1, s_state.lost_ip_count);
    TEST_ASSERT_EQUAL_INT64(5000000, s_state.last_lost_ip_us);
    TEST_ASSERT_EQUAL(0, other_ctx_policy.lost_ip_count);
    TEST_ASSERT_EQUAL_INT64(0, other_ctx_policy.last_lost_ip_us);
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

// ===========================================================================
// bb_fsm rebuild (B1-805 slice 1a) -- reachability tests. Drive the REAL FSM
// table (wifi_reconn_fsm_init) via a fake adapter recording every
// side-effecting call. Every fixture starts in WR_CONNECTING (never posts a
// synthetic GOT_IP first) -- this is the property the two prior wifi_reconn
// "fixes" silently failed to exercise.
// ===========================================================================

// esp_wifi WIFI_REASON_* wire values used below (bb_wifi_map_esp_reason,
// R13): 15 = 4WAY_HANDSHAKE_TIMEOUT (handshake bucket), 4 =
// DISASSOC_DUE_TO_INACTIVITY (generic bucket).
#define FSM_HANDSHAKE_ESP_REASON ((uint8_t)15)
#define FSM_GENERIC_ESP_REASON   ((uint8_t)4)

typedef struct {
    int64_t now_us;
    bool    reboot_allowed;
    int     connect_calls;
    int     disconnect_calls;
    int     reboot_calls;
    int     emit_calls;
    bb_wifi_net_event_t   last_emit_evt;
    bb_wifi_disc_reason_t last_emit_reason;
} fsm_fake_t;

static fsm_fake_t s_fake;

static int64_t fsm_fake_now_us(void) { return s_fake.now_us; }
static void    fsm_fake_connect(void) { s_fake.connect_calls++; }
static void    fsm_fake_disconnect(void) { s_fake.disconnect_calls++; }
static bool    fsm_fake_reboot_allowed(void) { return s_fake.reboot_allowed; }
static void    fsm_fake_reboot(const char *detail) { (void)detail; s_fake.reboot_calls++; }
static void    fsm_fake_emit(bb_wifi_net_event_t evt, bb_wifi_disc_reason_t reason)
{
    s_fake.emit_calls++;
    s_fake.last_emit_evt = evt;
    s_fake.last_emit_reason = reason;
}

static const wifi_reconn_adapter_t fsm_adapter = {
    .now_us                  = fsm_fake_now_us,
    .connect_fn              = fsm_fake_connect,
    .disconnect_fn           = fsm_fake_disconnect,
    .reboot_allowed_fn       = fsm_fake_reboot_allowed,
    .reboot_fn               = fsm_fake_reboot,
    .emit_net_event_fn       = fsm_fake_emit,
};

// Fresh fixture: fake reset (default: reboot allowed -- the common/allowed
// case; individual tests flip it to exercise the denied branch), ctx
// zeroed, FSM initialized at `initial` (WR_CONNECTING for every
// reachability test except the no-creds park test, which uses WR_NO_CREDS).
static void fsm_fixture_init(wifi_reconn_ctx_t *ctx, bb_fsm_state_t initial)
{
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.now_us = 1000000;
    s_fake.reboot_allowed = true;

    memset(ctx, 0, sizeof(*ctx));
    ctx->adapter = &fsm_adapter;
    TEST_ASSERT_EQUAL(BB_OK, wifi_reconn_fsm_init(ctx, initial));
}

// Row 5: WR_CONNECTING + DISCONNECT, within the fast-retry tier -> SAME,
// immediate reconnect, CONNECTING watchdog explicitly re-armed (SAME does
// not re-run on_entry).
void test_fsm_disconnect_reconnect_now_same_transition_rearms_watchdog(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);

    uint8_t reason = FSM_GENERIC_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.connect_calls);

    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_CONNECTING_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_CONNECTING_TIMEOUT_MS, tms);
}

// Row 6 -> WR_BACKOFF.on_entry -> Row 11 back to WR_CONNECTING. Ladder tier2
// backoff, then elapse -> re-arm CONNECTING watchdog.
void test_fsm_ladder_reaches_backoff_with_correct_timer(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    ctx.policy.handshake_fail_count = WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT;

    uint8_t reason = FSM_HANDSHAKE_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_BACKOFF, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.connect_calls);

    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_BACKOFF_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS, tms);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_BACKOFF_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.connect_calls);
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_CONNECTING_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_CONNECTING_TIMEOUT_MS, tms);
}

// Row 4: escalate reached ONLY after the persistent-fail window elapses.
void test_fsm_escalate_reached_after_persistent_fail_window(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);

    // First disconnect: not yet past the window -- ordinary tier1 SAME.
    uint8_t reason = FSM_GENERIC_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.reboot_calls);
    int64_t first_fail = ctx.policy.first_fail_us;
    TEST_ASSERT_NOT_EQUAL(0, first_fail);

    // Advance the fake clock past the window -- next disconnect escalates.
    s_fake.now_us = first_fail + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_ESCALATE_REBOOT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.reboot_calls);
}

// Row 4b (R14 guard-placement deny, via reboot_allowed_fn=false): ONE
// disconnect step lands in WR_BACKOFF, reboot_fn NOT called, REBOOT_DENIED
// emitted.
void test_fsm_disconnect_escalate_denied(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    ctx.policy.first_fail_us = 1000000;
    s_fake.now_us = 1000000 + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;
    s_fake.reboot_allowed = false;

    uint8_t reason = FSM_GENERIC_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_BACKOFF, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.reboot_calls);
    TEST_ASSERT_EQUAL(1, s_fake.emit_calls);
    TEST_ASSERT_EQUAL(BB_WIFI_NET_EVT_REBOOT_DENIED, s_fake.last_emit_evt);

    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_BACKOFF_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS, tms);
}

// Row 4, allowed: reboot_allowed_fn=true -> WR_ESCALATE_REBOOT, reboot_fn
// called once.
void test_fsm_disconnect_escalate_allowed(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    ctx.policy.first_fail_us = 1000000;
    s_fake.now_us = 1000000 + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;

    uint8_t reason = FSM_GENERIC_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_ESCALATE_REBOOT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.reboot_calls);
}

// Row 8: connect-timeout stall below the window -- re-attempt via
// disconnect_fn+connect_fn, stays WR_CONNECTING, watchdog re-armed (no
// restart_sta_fn adapter slot exists for this row -- removed as dead code,
// review fix [MEDIUM], B1-805 slice 1a).
void test_fsm_connecting_timeout_reattempts_without_teardown(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_CONNECTING_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.disconnect_calls);
    TEST_ASSERT_EQUAL(1, s_fake.connect_calls);
    TEST_ASSERT_TRUE(ctx.self_disconnect);

    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_CONNECTING_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_CONNECTING_TIMEOUT_MS, tms);
}

// Row 7: timeout-escalate allowed -> reboot.
void test_fsm_timeout_escalate_allowed(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    ctx.policy.first_fail_us = 1000000;
    s_fake.now_us = 1000000 + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_CONNECTING_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WR_ESCALATE_REBOOT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.reboot_calls);
}

// Row 7b, denied via reboot_allowed_fn=false.
void test_fsm_timeout_escalate_denied(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    ctx.policy.first_fail_us = 1000000;
    s_fake.now_us = 1000000 + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;
    s_fake.reboot_allowed = false;

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_CONNECTING_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WR_BACKOFF, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.reboot_calls);
    TEST_ASSERT_EQUAL(1, s_fake.emit_calls);
    TEST_ASSERT_EQUAL(BB_WIFI_NET_EVT_REBOOT_DENIED, s_fake.last_emit_evt);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_UNKNOWN, s_fake.last_emit_reason);
}

// Row 2: GOT_IP resets policy counters, transitions WR_CONNECTED, no timer.
void test_fsm_got_ip_resets_and_transitions_connected(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    ctx.policy.handshake_fail_count = 3;
    ctx.policy.first_fail_us = 12345;

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_GOT_IP, NULL));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTED, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, ctx.policy.handshake_fail_count);
    TEST_ASSERT_EQUAL_INT64(0, ctx.policy.first_fail_us);
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&ctx.fsm));
}

// Row 3: STA_CONNECTED is log-only -- SAME, no action side effects.
void test_fsm_connecting_sta_connected_is_log_only_same(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_CONNECTED, NULL));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.connect_calls);
}

// Row 9: WR_CONNECTED + DISCONNECT within fast-retry tier -> WR_CONNECTING,
// watchdog re-armed by WR_CONNECTING's own on_entry (concrete transition).
void test_fsm_connected_disconnect_reconnect_now_to_connecting(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_GOT_IP, NULL));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTED, bb_fsm_state(&ctx.fsm));

    uint8_t reason = FSM_GENERIC_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.connect_calls);

    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_CONNECTING_TIMEOUT, tev);
}

// Row 10: WR_CONNECTED + DISCONNECT beyond fast-retry tier -> WR_BACKOFF.
void test_fsm_connected_disconnect_backoff_to_backoff(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_GOT_IP, NULL));
    ctx.policy.generic_fail_count = WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT;

    uint8_t reason = FSM_GENERIC_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_BACKOFF, bb_fsm_state(&ctx.fsm));

    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_BACKOFF_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS, tms);
}

// Row 12: WR_BACKOFF absorbs a stray DISCONNECTED (SAME, no-op).
void test_fsm_backoff_absorbs_stray_disconnect(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    ctx.policy.generic_fail_count = WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT;
    uint8_t reason = FSM_GENERIC_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_BACKOFF, bb_fsm_state(&ctx.fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_BACKOFF, bb_fsm_state(&ctx.fsm));
}

// Row 1 + the no-creds safety invariant (BINDING, test #7 in the spec):
// WR_NO_CREDS is terminal-until-provisioned. Every stray event is a safe
// no-op (BB_ERR_NOT_FOUND, state unchanged, no side effects, no timer);
// only EV_CREDS_ARRIVED moves it, to WR_CONNECTING.
void test_fsm_no_creds_parks_until_creds_arrived(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_NO_CREDS);
    s_fake.now_us += WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;

    uint8_t reason = FSM_GENERIC_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_NO_CREDS, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_NO_CREDS, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_CONNECTING_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WR_NO_CREDS, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_BACKOFF_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WR_NO_CREDS, bb_fsm_state(&ctx.fsm));

    TEST_ASSERT_EQUAL(0, s_fake.connect_calls);
    TEST_ASSERT_EQUAL(0, s_fake.reboot_calls);
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&ctx.fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_CREDS_ARRIVED, NULL));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.connect_calls);
    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_CONNECTING_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_CONNECTING_TIMEOUT_MS, tms);
}

// ===========================================================================
// PR7 (B1-994/B1-806) -- ASSOC_LEAVE park (WR_LEFT) + SLOW backoff tier FSM
// reachability. esp reason 8 = WIFI_REASON_ASSOC_LEAVE (bb_wifi_map_esp_reason).
// ===========================================================================

#define FSM_ASSOC_LEAVE_ESP_REASON ((uint8_t)8)
#define FSM_AUTH_FAIL_ESP_REASON   ((uint8_t)2)   // WIFI_REASON_AUTH_EXPIRE

// New WR_LEFT row (WR_CONNECTING): ASSOC_LEAVE parks BEFORE any escalate/
// reconnect-now/backoff row matches -- no connect, no timer, RECONNECT_PARKED
// emitted.
void test_fsm_assoc_leave_from_connecting_parks_no_reconnect(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);

    uint8_t reason = FSM_ASSOC_LEAVE_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_LEFT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.connect_calls);
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.emit_calls);
    TEST_ASSERT_EQUAL(BB_WIFI_NET_EVT_RECONNECT_PARKED, s_fake.last_emit_evt);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_ASSOC_LEAVE, s_fake.last_emit_reason);
}

// Same row on WR_CONNECTED.
void test_fsm_assoc_leave_from_connected_parks_no_reconnect(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_GOT_IP, NULL));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTED, bb_fsm_state(&ctx.fsm));

    uint8_t reason = FSM_ASSOC_LEAVE_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_LEFT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.connect_calls);
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.emit_calls);
    TEST_ASSERT_EQUAL(BB_WIFI_NET_EVT_RECONNECT_PARKED, s_fake.last_emit_evt);
}

// REQUIRED negative test: conditions that would escalate ANY other reason
// (stale first_fail past the persistent-fail window) -- an ASSOC_LEAVE
// disconnect still parks to WR_LEFT, never WR_ESCALATE_REBOOT. The
// escalate-allowed row is reason-independent, but the ASSOC_LEAVE row is
// ordered BEFORE it in the table, so it always wins the match.
void test_fsm_assoc_leave_does_not_escalate_past_fail_window(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);
    ctx.policy.first_fail_us = 1000000;
    s_fake.now_us = 1000000 + WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US + 1000000;

    uint8_t reason = FSM_ASSOC_LEAVE_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_LEFT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.reboot_calls);
}

// From WR_LEFT, any stray event (other than resume) is a safe no-op:
// BB_ERR_NOT_FOUND, state unchanged, no side effects, no timer.
void test_fsm_left_parks_all_stray_events(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_LEFT);

    uint8_t reason = FSM_AUTH_FAIL_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_LEFT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_GOT_IP, NULL));
    TEST_ASSERT_EQUAL_INT(WR_LEFT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_CONNECTING_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WR_LEFT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_BACKOFF_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WR_LEFT, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_CREDS_ARRIVED, NULL));
    TEST_ASSERT_EQUAL_INT(WR_LEFT, bb_fsm_state(&ctx.fsm));

    TEST_ASSERT_EQUAL(0, s_fake.connect_calls);
    TEST_ASSERT_EQUAL(0, s_fake.disconnect_calls);
    TEST_ASSERT_EQUAL(0, s_fake.reboot_calls);
    TEST_ASSERT_EQUAL(0, s_fake.emit_calls);
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&ctx.fsm));
}

// WR_LEFT resume row: EV_RECONNECT_REQUESTED -> WR_CONNECTING, connect_fn
// called once, CONNECTING watchdog armed (via WR_CONNECTING's on_entry),
// counters reset (act_reset_state -- same action as WR_NO_CREDS's resume).
void test_fsm_left_resumes_on_reconnect_requested(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_LEFT);
    ctx.policy.slow_fail_count = 3;
    ctx.policy.generic_fail_count = 2;

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_RECONNECT_REQUESTED, NULL));
    TEST_ASSERT_EQUAL_INT(WR_CONNECTING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.connect_calls);
    TEST_ASSERT_EQUAL(0, ctx.policy.slow_fail_count);
    TEST_ASSERT_EQUAL(0, ctx.policy.generic_fail_count);

    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_CONNECTING_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_CONNECTING_TIMEOUT_MS, tms);
}

// SLOW tier drives the FSM to WR_BACKOFF with the tier-1 timer on a fresh
// AUTH_FAIL disconnect (never hot-retries via guard_disc_reconnect_now).
void test_fsm_slow_tier_disconnect_routes_to_backoff(void)
{
    wifi_reconn_ctx_t ctx;
    fsm_fixture_init(&ctx, WR_CONNECTING);

    uint8_t reason = FSM_AUTH_FAIL_ESP_REASON;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_STA_DISCONNECTED, &reason));
    TEST_ASSERT_EQUAL_INT(WR_BACKOFF, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.connect_calls);

    bb_fsm_event_t tev;
    uint32_t tms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &tev, &tms));
    TEST_ASSERT_EQUAL_INT(EV_BACKOFF_TIMEOUT, tev);
    TEST_ASSERT_EQUAL_UINT32(WIFI_RECONN_SLOW_BACKOFF_TIER1_MS, tms);
}
