#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Host-only test hooks for bb_wifi.
 * Only available when BB_WIFI_TESTING is defined.
 */
#ifdef BB_WIFI_TESTING
void bb_wifi_test_set_has_ip(bool has_ip);
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

// Inject a full 256-entry reason histogram (host only).
// len entries are copied; remaining buckets are zeroed.
void bb_wifi_test_set_reason_histogram(const uint16_t *hist, size_t len);
#endif
