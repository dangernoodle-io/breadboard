#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Host-safe types (usable outside #ifdef ESP_PLATFORM)
#define WIFI_SCAN_MAX 20

typedef enum {
    BSP_WIFI_STATE_DISCONNECTED,
    BSP_WIFI_STATE_CONNECTING,
    BSP_WIFI_STATE_CONNECTED,
    BSP_WIFI_STATE_AP_MODE
} bsp_wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;   // true if not WIFI_AUTH_OPEN
} bsp_wifi_ap_t;

#ifdef ESP_PLATFORM

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ESP32-specific functions and types

// Performs a blocking WiFi scan. Returns number of APs found (up to max_results).
int bsp_wifi_scan_networks(bsp_wifi_ap_t *results, int max_results);

// Start a non-blocking WiFi scan in the background. Returns immediately.
void bsp_wifi_scan_start_async(void);

// Get cached WiFi scan results. Returns number of APs found (0 if no scan done yet).
int bsp_wifi_scan_get_cached(bsp_wifi_ap_t *results, int max_results);

// STA mode — blocks until connected or timeout
esp_err_t bsp_wifi_init(void);           // restarts on timeout (normal boot)
esp_err_t bsp_wifi_init_sta(void);       // returns ESP_ERR_TIMEOUT on failure (provisioning retry)

// Force WiFi reassociation — recover from zombie-connected state
void bsp_wifi_force_reassociate(void);

// AP mode — for provisioning
esp_err_t bsp_wifi_init_ap(void);        // starts AP + captive DNS
void bsp_wifi_stop_ap(void);             // stops AP + DNS, deinits wifi
void bsp_wifi_prov_get_ap_ssid(char *buf, size_t len);  // get AP SSID

// Provisioning event
#define PROV_DONE_BIT BIT0
extern EventGroupHandle_t g_prov_event_group;

// Diagnostic getters — lock-free reads from interrupt-safe statics
void bsp_wifi_prov_get_disconnect(uint8_t *reason, int64_t *age_us);
int bsp_wifi_prov_get_retry_count(void);
bool bsp_wifi_prov_mdns_started(void);
esp_err_t bsp_wifi_prov_get_ip_str(char *out, size_t out_len);
esp_err_t bsp_wifi_prov_get_rssi(int8_t *out);
bool bsp_wifi_prov_has_ip(void);

// App-injected mDNS hostname setter
void bsp_wifi_set_mdns_hostname(const char *hostname);

#endif /* ESP_PLATFORM */
