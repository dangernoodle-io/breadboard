#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Host-safe types (usable outside #ifdef ESP_PLATFORM)
#define WIFI_SCAN_MAX 20

typedef enum {
    BB_WIFI_STATE_DISCONNECTED,
    BB_WIFI_STATE_CONNECTING,
    BB_WIFI_STATE_CONNECTED,
    BB_WIFI_STATE_AP_MODE
} bb_wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;   // true if not WIFI_AUTH_OPEN
} bb_wifi_ap_t;

#ifdef ESP_PLATFORM

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

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

// AP mode — for provisioning
esp_err_t bb_wifi_init_ap(void);        // starts AP + captive DNS
void bb_wifi_stop_ap(void);             // stops AP + DNS, deinits wifi
void bb_wifi_prov_get_ap_ssid(char *buf, size_t len);  // get AP SSID

// Provisioning event
#define PROV_DONE_BIT BIT0
extern EventGroupHandle_t g_prov_event_group;

// Diagnostic getters — lock-free reads from interrupt-safe statics
void bb_wifi_prov_get_disconnect(uint8_t *reason, int64_t *age_us);
int bb_wifi_prov_get_retry_count(void);
bool bb_wifi_prov_mdns_started(void);
esp_err_t bb_wifi_prov_get_ip_str(char *out, size_t out_len);
esp_err_t bb_wifi_prov_get_rssi(int8_t *out);
bool bb_wifi_prov_has_ip(void);

// App-injected mDNS hostname setter
void bb_wifi_set_mdns_hostname(const char *hostname);

#endif /* ESP_PLATFORM */
