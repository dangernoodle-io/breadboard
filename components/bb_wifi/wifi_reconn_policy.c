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
    for (int i = 0; i < 256; i++) {
        st->reason_histogram[i] = 0;
    }
}

static uint32_t compute_backoff_ms(const wifi_reconn_state_t *st, uint8_t reason,
                                    uint8_t handshake_reason_code)
{
    if (reason == handshake_reason_code) {
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
    uint8_t reason, uint8_t handshake_reason_code,
    uint32_t *backoff_ms_out)
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
    if (reason == handshake_reason_code) {
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
    uint32_t backoff_ms = compute_backoff_ms(st, reason, handshake_reason_code);
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
    // Pass distinct reason/handshake values to force the generic branch.
    uint32_t backoff_ms = compute_backoff_ms(st, 0, 1);
    *backoff_ms_out = backoff_ms;

    if (backoff_ms == 0) {
        return WIFI_RECONN_ACTION_RECONNECT_NOW;
    }

    return WIFI_RECONN_ACTION_SCHEDULE_BACKOFF;
}
