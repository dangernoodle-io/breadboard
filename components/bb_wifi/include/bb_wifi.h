#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Host-safe types (usable outside #ifdef ESP_PLATFORM)
#define WIFI_SCAN_MAX 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;   // true if not WIFI_AUTH_OPEN
} bb_wifi_ap_t;

#ifdef ESP_PLATFORM

#include "esp_err.h"

// ESP32-specific functions and types

// Performs a blocking WiFi scan. Returns number of APs found (up to max_results).
int bb_wifi_scan_networks(bb_wifi_ap_t *results, int max_results);

// Start a non-blocking WiFi scan in the background. Returns immediately.
void bb_wifi_scan_start_async(void);

// Get cached WiFi scan results. Returns number of APs found (0 if no scan done yet).
int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results);

// STA mode — blocks until connected or timeout
esp_err_t bb_wifi_init(void);           // restarts on timeout (normal boot)
esp_err_t bb_wifi_init_sta(void);       // returns ESP_ERR_TIMEOUT on failure (provisioning retry)

// Force WiFi reassociation — recover from zombie-connected state
void bb_wifi_force_reassociate(void);

// Got-IP callback registration
typedef void (*bb_wifi_on_got_ip_cb_t)(void);
void bb_wifi_register_on_got_ip(bb_wifi_on_got_ip_cb_t cb);

// Diagnostic getters — lock-free reads from interrupt-safe statics
void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us);
int bb_wifi_get_retry_count(void);
esp_err_t bb_wifi_get_ip_str(char *out, size_t out_len);
esp_err_t bb_wifi_get_rssi(int8_t *out);
bool bb_wifi_has_ip(void);

#endif /* ESP_PLATFORM */
