#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Breadboard sentinel: reason code injected into reason_histogram when IP is lost
// while still associated. 99 is free in esp_wifi_types.h (reasons: 1-24, 53-67,
// 200-208) and fits in uint8_t (< 256, the histogram size).
#define WIFI_REASON_BB_LOST_IP 99

#define WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS 10000
#define WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS 30000
#define WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS   5000
#define WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT   10
#define WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT 3
#define WIFI_RECONN_HANDSHAKE_TIER2_LIMIT      6
#ifndef CONFIG_BB_WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S
#define CONFIG_BB_WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S 300
#endif
#define WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US  ((int64_t)CONFIG_BB_WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S * 1000000LL)

typedef struct {
    int      handshake_fail_count;
    int      generic_fail_count;
    int64_t  first_fail_us;
    int      retry_count;
    uint8_t  last_reason;
    int64_t  last_disconnect_us;
    uint16_t reason_histogram[256];
    uint32_t lost_ip_count;    // times lost IP while associated
    int64_t  last_lost_ip_us;  // timestamp of last lost-IP event
} wifi_reconn_state_t;

typedef struct {
    int64_t (*now_us)(void);
} wifi_reconn_adapter_t;

typedef enum {
    WIFI_RECONN_ACTION_NONE,
    WIFI_RECONN_ACTION_RECONNECT_NOW,
    WIFI_RECONN_ACTION_SCHEDULE_BACKOFF,
    WIFI_RECONN_ACTION_REBOOT,
} wifi_reconn_action_t;

// Reset all counters to zero.
void wifi_reconn_state_reset(wifi_reconn_state_t *st);

// Policy decision on disconnect.
// Caller passes the platform's HANDSHAKE_TIMEOUT reason code so this
// module stays free of esp_wifi_types.h.
// Returns action enum; if SCHEDULE_BACKOFF, populates *backoff_ms_out.
wifi_reconn_action_t wifi_reconn_policy_on_disconnect(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a,
    uint8_t reason, uint8_t handshake_reason_code,
    uint32_t *backoff_ms_out);

// Reset counters on successful IP acquisition.
void wifi_reconn_policy_on_got_ip(wifi_reconn_state_t *st);

// Return true when the board is L2-associated but has no DHCP IP — the zombie state.
bool wifi_reconn_should_reconnect_no_ip(bool associated, bool has_ip);

// Record a lost-IP event in policy state (bumps lost_ip_count, last_lost_ip_us,
// reason_histogram[WIFI_REASON_BB_LOST_IP]). Guards null args.
void wifi_reconn_policy_on_lost_ip(wifi_reconn_state_t *st, const wifi_reconn_adapter_t *ad);

// Policy decision when a connect attempt stalls (no GOT_IP or DISCONNECT
// within the connecting watchdog window). Mirrors on_disconnect escalation:
// bumps generic_fail_count, sets first_fail_us if 0, increments retry_count.
// Returns WIFI_RECONN_ACTION_REBOOT after WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US;
// otherwise applies the same progressive generic backoff as on_disconnect
// (RECONNECT_NOW within GENERIC_FAST_RETRY_LIMIT, SCHEDULE_BACKOFF beyond it).
wifi_reconn_action_t wifi_reconn_policy_on_connect_timeout(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a,
    uint32_t *backoff_ms_out);
