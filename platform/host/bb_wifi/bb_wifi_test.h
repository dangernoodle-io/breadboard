#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bb_wifi.h"

/**
 * Host-only test hooks for bb_wifi.
 * Only available when BB_WIFI_TESTING is defined.
 */
#ifdef BB_WIFI_TESTING
void bb_wifi_test_set_has_ip(bool has_ip);
void bb_wifi_test_set_associated(bool associated);
void bb_wifi_test_set_recovery_blocked(bool blocked);
int  bb_wifi_test_get_recovery_count(void);
const char *bb_wifi_test_get_last_recovery_reason(void);
void bb_wifi_test_reset_recovery(void);

// Setters for the three new recovery-telemetry fields.
void bb_wifi_test_set_restart_sta_count(uint32_t count);
void bb_wifi_test_set_disconnect_rssi(int8_t rssi);

// Setters for the roam/BSSID-change counter (B1-497, observe-only).
void bb_wifi_test_set_roam_count(uint32_t count);
void bb_wifi_test_set_roam_age_s(uint32_t age_s);

// Setter for the connected-session-duration telemetry (observe-only).
void bb_wifi_test_set_last_session_s(uint32_t session_s);

// Inject a full 256-entry reason histogram (host only).
// len entries are copied; remaining buckets are zeroed.
void bb_wifi_test_set_reason_histogram(const uint16_t *hist, size_t len);

// Drive bb_wifi_get_gateway_status() on host (B1-518 PR2, observe-only
// gateway probe). Pass NULL to clear back to the default zeroed status.
void bb_wifi_host_set_gateway_status(const bb_wifi_gw_status_t *status);
#endif
