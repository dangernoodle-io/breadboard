// bb_pub_wifi — telemetry source satellite for bb_wifi.
//
// Registers a bb_pub source under the "wifi" subtopic. On each tick,
// samples bb_wifi_get_info() and emits the full connection snapshot:
//   ssid        — string (SSID of associated AP)
//   bssid       — string (colon-separated hex, e.g. "aa:bb:cc:dd:ee:ff")
//   rssi        — integer dBm (signal strength)
//   ip          — string (dotted-quad IPv4)
//   connected   — bool
//   disc_reason — integer (last disconnect reason code)
//   disc_age_s  — integer (seconds since last disconnect, 0 if never)
//   retry_count — integer (STA retry attempts since last connect)
//
// Returns false (skip) when WiFi STA is not connected (bb_wifi_has_ip()
// is false) — no publish during disconnects.
//
// Self-registration is gated on CONFIG_BB_PUB_WIFI_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP
// tier after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "wifi" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_WIFI_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_wifi_register(void);

// ---------------------------------------------------------------------------
// Host test hooks (only when BB_PUB_WIFI_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_PUB_WIFI_TESTING

/** Stub wifi info for host tests — mirrors bb_wifi_info_t fields. */
typedef struct {
    bool     connected;
    int8_t   rssi;
    char     ssid[33];
    uint8_t  bssid[6];
    char     ip[16];
    uint8_t  disc_reason;
    uint32_t disc_age_s;
    int      retry_count;
} bb_pub_wifi_test_info_t;

/**
 * Set the stub wifi info used by the host sample function.
 * Pass connected=false to simulate a disconnected state (source returns false).
 */
void bb_pub_wifi_test_set_info(const bb_pub_wifi_test_info_t *info);

/** Convenience wrapper — sets rssi only (legacy helper, bssid/ip/etc zeroed). */
void bb_pub_wifi_test_set_rssi(bool connected, int8_t rssi);

#endif /* BB_PUB_WIFI_TESTING */

#ifdef __cplusplus
}
#endif
