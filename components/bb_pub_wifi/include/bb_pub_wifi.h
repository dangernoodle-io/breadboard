// bb_pub_wifi — telemetry source satellite for bb_wifi.
//
// Registers a bb_pub source under the "wifi" subtopic. On each tick,
// samples bb_wifi_get_rssi() and emits:
//   rssi   — integer dBm (signal strength)
//
// Returns false (skip) when WiFi STA is not connected (bb_wifi_has_ip()
// is false) — no publish during disconnects.
//
// Self-registration is gated on CONFIG_BB_PUB_WIFI_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP
// tier after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"

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

/** Set the stub rssi and connected flag used by the host sample function. */
void bb_pub_wifi_test_set_rssi(bool connected, int8_t rssi);

#endif /* BB_PUB_WIFI_TESTING */

#ifdef __cplusplus
}
#endif
