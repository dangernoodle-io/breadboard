#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Public types — usable on every backend.
// ---------------------------------------------------------------------------

#define WIFI_SCAN_MAX 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;   // true if not WIFI_AUTH_OPEN
} bb_wifi_ap_t;

// Snapshot of the current STA connection. Populated by bb_wifi_get_info.
// On backends that don't surface a given field, it is zeroed.
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

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Idempotent one-shot bring-up of the underlying network stack. On ESP-IDF
// this initializes esp_netif and the default event loop. On backends where
// the platform handles this implicitly (Arduino), this is a BB_OK no-op so
// portable consumer code can call it unconditionally.
bb_err_t bb_wifi_ensure_netif(void);

// STA mode connect. bb_wifi_init restarts the system on connect timeout
// (intended for normal boot); bb_wifi_init_sta returns an error on timeout
// instead (intended for provisioning retry loops). Both block until
// connected or timeout.
bb_err_t bb_wifi_init(void);
bb_err_t bb_wifi_init_sta(void);

// Force WiFi reassociation — recover from a zombie-connected state.
// On backends without explicit reassociation control (Arduino), this is
// a BB_OK no-op.
void bb_wifi_force_reassociate(void);

// ---------------------------------------------------------------------------
// Hostname
// ---------------------------------------------------------------------------

// Set the DHCP host name (Option 12) advertised by the STA netif.
// Independent of mDNS — call bb_mdns_set_hostname() separately if you
// want the same value on both surfaces. Safe to call before STA connects;
// the value is sent on the next DHCP DISCOVER/REQUEST. Returns
// BB_ERR_INVALID_ARG on NULL/empty hostname; BB_ERR_INVALID_STATE if
// the STA netif isn't initialized yet. On backends without hostname
// support (e.g. CC3000), returns BB_OK no-op.
bb_err_t bb_wifi_set_hostname(const char *hostname);

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------
// On backends without scan support, the blocking variant returns 0 and the
// async pair returns BB_OK / 0 — callers can invoke them unconditionally.

// Blocking WiFi scan. Returns number of APs found (up to max_results).
int bb_wifi_scan_networks(bb_wifi_ap_t *results, int max_results);

// Start a non-blocking WiFi scan in the background. Returns immediately.
void bb_wifi_scan_start_async(void);

// Get cached WiFi scan results. Returns number of APs (0 if no scan done).
int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results);

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
// Backends that lack a native event loop (Arduino) dispatch callbacks
// synchronously from inside bb_wifi_init_sta and bb_http_server_poll
// transitions, but the contract is unchanged from the consumer's POV.

typedef void (*bb_wifi_on_got_ip_cb_t)(void);
void bb_wifi_register_on_got_ip(bb_wifi_on_got_ip_cb_t cb);

typedef void (*bb_wifi_on_disconnect_cb_t)(void);
void bb_wifi_register_on_disconnect(bb_wifi_on_disconnect_cb_t cb);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us);
int  bb_wifi_get_retry_count(void);
bb_err_t bb_wifi_get_ip_str(char *out, size_t out_len);
bb_err_t bb_wifi_get_rssi(int8_t *out);
bool bb_wifi_has_ip(void);

// Populate out with a snapshot of the current STA state. Fields the
// backend cannot supply are zeroed. Returns BB_ERR_INVALID_ARG on null.
bb_err_t bb_wifi_get_info(bb_wifi_info_t *out);

// ---------------------------------------------------------------------------
// Transport (Arduino bb_http only)
// ---------------------------------------------------------------------------
// Generic non-blocking accept-loop transport, consumed by the Arduino
// bb_http backend so it can stay radio-agnostic. ESP-IDF bb_http uses
// esp_http_server directly and the ESP-IDF backend stubs these out as
// BB_ERR_INVALID_STATE.

typedef struct bb_conn bb_conn_t;

// Begin listening on port. Idempotent. The backend may defer the actual
// listen syscall until the first bb_wifi_accept call (CC3000 in particular
// requires server construction from loop() context, not init).
bb_err_t bb_wifi_listen(uint16_t port);

// Non-blocking accept. Returns BB_OK with *out set to a connection handle,
// or *out == NULL if no connection is pending. The handle is owned by the
// backend; release it with bb_conn_close.
bb_err_t bb_wifi_accept(bb_conn_t **out);

// Number of bytes available to read without blocking.
int  bb_conn_available(bb_conn_t *c);

// Read up to n bytes. Returns bytes read, 0 if none available, -1 on error.
int  bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n);

// Write up to n bytes. Returns bytes written, -1 on error.
int  bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n);

// Close the connection and release the handle.
void bb_conn_close(bb_conn_t *c);

#ifdef __cplusplus
}
#endif
