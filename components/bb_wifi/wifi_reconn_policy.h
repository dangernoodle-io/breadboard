#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS 10000
#define WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS 30000
#define WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS   5000
#define WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT   10
#define WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT 3
#define WIFI_RECONN_HANDSHAKE_TIER2_LIMIT      6
#define WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US  (5LL * 60LL * 1000000LL)

typedef struct {
    int      handshake_fail_count;
    int      generic_fail_count;
    int64_t  first_fail_us;
    int      retry_count;
    uint8_t  last_reason;
    int64_t  last_disconnect_us;
    uint16_t reason_histogram[256];
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
