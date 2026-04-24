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

// Snapshot of the current STA connection. Populated by bb_wifi_get_info.
// On non-ESP backends, unavailable fields are zeroed.
typedef struct {
    char ssid[33];       // SSID of associated AP, empty if not connected
    uint8_t bssid[6];    // BSSID of associated AP
    int8_t rssi;         // signal strength, 0 if not connected
    char ip[16];         // dotted-quad IPv4, "0.0.0.0" if no IP
    bool connected;      // true iff has_ip
    uint8_t disc_reason; // last disconnect reason code
    uint32_t disc_age_s; // seconds since last disconnect, 0 if never
    int retry_count;     // STA retry attempts since last connect
} bb_wifi_info_t;

#ifdef ESP_PLATFORM

#include "bb_nv.h"

// ESP32-specific functions and types

// Idempotent one-shot: esp_netif_init + esp_event_loop_create_default.
// Safe to call from multiple components; only initializes once.
bb_err_t bb_wifi_ensure_netif(void);

// Performs a blocking WiFi scan. Returns number of APs found (up to max_results).
int bb_wifi_scan_networks(bb_wifi_ap_t *results, int max_results);

// Start a non-blocking WiFi scan in the background. Returns immediately.
void bb_wifi_scan_start_async(void);

// Get cached WiFi scan results. Returns number of APs found (0 if no scan done yet).
int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results);

// STA mode — blocks until connected or timeout
bb_err_t bb_wifi_init(void);           // restarts on timeout (normal boot)
bb_err_t bb_wifi_init_sta(void);       // returns BB_ERR_TIMEOUT on failure (provisioning retry)

// Force WiFi reassociation — recover from zombie-connected state
void bb_wifi_force_reassociate(void);

// Got-IP callback registration
typedef void (*bb_wifi_on_got_ip_cb_t)(void);
void bb_wifi_register_on_got_ip(bb_wifi_on_got_ip_cb_t cb);

// Disconnect callback registration
typedef void (*bb_wifi_on_disconnect_cb_t)(void);
void bb_wifi_register_on_disconnect(bb_wifi_on_disconnect_cb_t cb);

// Diagnostic getters — lock-free reads from interrupt-safe statics
void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us);
int bb_wifi_get_retry_count(void);
bb_err_t bb_wifi_get_ip_str(char *out, size_t out_len);
bb_err_t bb_wifi_get_rssi(int8_t *out);
bool bb_wifi_has_ip(void);

// Populate out with a snapshot of the current STA state. Safe to call any
// time; unset fields are zeroed. Returns BB_OK on success, BB_ERR_* on
// null arg.
bb_err_t bb_wifi_get_info(bb_wifi_info_t *out);

// Register GET /api/wifi on server. Returns bb_wifi_get_info() snapshot
// as JSON. `server` is bb_http_handle_t; declared as void* to avoid
// pulling http_server.h into bb_wifi consumers.
bb_err_t bb_wifi_register_routes(void *server);

#endif /* ESP_PLATFORM */
