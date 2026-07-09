#include "wifi_reconn_policy.h"

void wifi_reconn_state_reset(wifi_reconn_state_t *st)
{
    if (!st) return;
    st->handshake_fail_count = 0;
    st->generic_fail_count = 0;
    st->first_fail_us = 0;
    st->retry_count = 0;
    st->last_reason = 0;
    st->last_disconnect_us = 0;
    st->lost_ip_count = 0;
    st->last_lost_ip_us = 0;
    st->egress_fail_streak = 0;
    st->egress_dead_count = 0;
    st->last_egress_dead_us = 0;
    st->no_ip_count = 0;
    st->last_no_ip_us = 0;
    for (int i = 0; i < BB_WIFI_DISC_COUNT; i++) {
        st->reason_histogram[i] = 0;
    }
}

static uint32_t compute_backoff_ms(const wifi_reconn_state_t *st, bb_wifi_disc_reason_t reason)
{
    if (reason == BB_WIFI_DISC_HANDSHAKE_TIMEOUT) {
        int n = st->handshake_fail_count;
        if (n <= WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT)
            return 0;
        if (n <= WIFI_RECONN_HANDSHAKE_TIER2_LIMIT)
            return WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS;
        return WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS;
    }
    int n = st->generic_fail_count;
    if (n <= WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT)
        return 0;
    return WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS;
}

wifi_reconn_action_t wifi_reconn_policy_on_disconnect(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a,
    bb_wifi_disc_reason_t reason, uint32_t *backoff_ms_out)
{
    if (!st || !a || !backoff_ms_out) {
        return WIFI_RECONN_ACTION_NONE;
    }

    int64_t now = a->now_us();

    // Update diagnostic state.
    st->last_reason = reason;
    st->last_disconnect_us = now;
    if (st->reason_histogram[reason] < UINT16_MAX) {
        st->reason_histogram[reason]++;
    }
    st->retry_count++;

    // Track first failure time in current window.
    if (st->first_fail_us == 0) {
        st->first_fail_us = now;
    }

    // Increment appropriate counter based on reason.
    if (reason == BB_WIFI_DISC_HANDSHAKE_TIMEOUT) {
        st->handshake_fail_count++;
    } else {
        st->generic_fail_count++;
    }

    // Check if we've exceeded the persistent fail window. first_fail_us is
    // guaranteed non-zero here (set above when 0).
    if (now - st->first_fail_us > WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US) {
        return WIFI_RECONN_ACTION_REBOOT;
    }

    // Compute backoff strategy.
    uint32_t backoff_ms = compute_backoff_ms(st, reason);
    *backoff_ms_out = backoff_ms;

    if (backoff_ms == 0) {
        return WIFI_RECONN_ACTION_RECONNECT_NOW;
    }

    return WIFI_RECONN_ACTION_SCHEDULE_BACKOFF;
}

void wifi_reconn_policy_on_got_ip(wifi_reconn_state_t *st)
{
    if (!st) return;
    st->handshake_fail_count = 0;
    st->generic_fail_count = 0;
    st->first_fail_us = 0;
    st->retry_count = 0;
}

bool wifi_reconn_should_reconnect_no_ip(bool associated, bool has_ip)
{
    return associated && !has_ip;
}

void wifi_reconn_policy_on_lost_ip(wifi_reconn_state_t *st, const wifi_reconn_adapter_t *ad)
{
    if (!st || !ad) return;
    st->lost_ip_count++;
    st->last_lost_ip_us = ad->now_us();
    if (st->reason_histogram[WIFI_REASON_BB_LOST_IP] < UINT16_MAX) {
        st->reason_histogram[WIFI_REASON_BB_LOST_IP]++;
    }
}

void wifi_reconn_policy_on_no_ip(wifi_reconn_state_t *st, const wifi_reconn_adapter_t *ad)
{
    if (!st || !ad) return;
    st->no_ip_count++;
    st->last_no_ip_us = ad->now_us();
    if (st->reason_histogram[WIFI_REASON_BB_NO_IP_WATCHDOG] < UINT16_MAX) {
        st->reason_histogram[WIFI_REASON_BB_NO_IP_WATCHDOG]++;
    }
    // Arm the persistent-fail window so this does not compound with the
    // 5-min safeguard-reboot unexpectedly (B1-353): first_fail_us resets on
    // GOT_IP via wifi_reconn_policy_on_got_ip, so a successful reconnect
    // after a no-IP watchdog restart clears the window cleanly.
    if (st->first_fail_us == 0) {
        st->first_fail_us = st->last_no_ip_us;
    }
}

wifi_reconn_action_t wifi_reconn_policy_on_connect_timeout(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a,
    uint32_t *backoff_ms_out)
{
    if (!st || !a || !backoff_ms_out) {
        return WIFI_RECONN_ACTION_NONE;
    }

    int64_t now = a->now_us();

    st->last_disconnect_us = now;
    st->retry_count++;
    st->generic_fail_count++;

    if (st->first_fail_us == 0) {
        st->first_fail_us = now;
    }

    if (now - st->first_fail_us > WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US) {
        return WIFI_RECONN_ACTION_REBOOT;
    }

    // Apply the same progressive backoff as on_disconnect's generic path.
    // BB_WIFI_DISC_UNKNOWN != BB_WIFI_DISC_HANDSHAKE_TIMEOUT, forcing the
    // generic branch (a connect-timeout stall is never a handshake reason).
    uint32_t backoff_ms = compute_backoff_ms(st, BB_WIFI_DISC_UNKNOWN);
    *backoff_ms_out = backoff_ms;

    if (backoff_ms == 0) {
        return WIFI_RECONN_ACTION_RECONNECT_NOW;
    }

    return WIFI_RECONN_ACTION_SCHEDULE_BACKOFF;
}

wifi_reconn_action_t wifi_reconn_policy_on_egress_probe(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *ad,
    bool reachable, int fail_threshold)
{
    if (!st || !ad) return WIFI_RECONN_ACTION_NONE;

    if (reachable) {
        st->egress_fail_streak = 0;
        return WIFI_RECONN_ACTION_NONE;
    }

    st->egress_fail_streak++;
    if (st->egress_fail_streak >= (uint8_t)fail_threshold) {
        st->egress_dead_count++;
        st->last_egress_dead_us = ad->now_us();
        if (st->reason_histogram[WIFI_REASON_BB_EGRESS_DEAD] < UINT16_MAX) {
            st->reason_histogram[WIFI_REASON_BB_EGRESS_DEAD]++;
        }
        if (st->first_fail_us == 0) {
            st->first_fail_us = ad->now_us();
        }
        st->egress_fail_streak = 0;
        return WIFI_RECONN_ACTION_RECONNECT_NOW;
    }
    return WIFI_RECONN_ACTION_NONE;
}
