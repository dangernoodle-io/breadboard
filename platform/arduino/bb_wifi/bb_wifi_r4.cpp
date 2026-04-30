// Arduino UNO R4 WiFi bb_wifi backend (WiFiS3 / on-board ESP32-S3 radio).
// Selected by -DBB_WIFI_BACKEND_R4 in the consumer's build_flags.
//
// Functions that are no-ops on this backend (because WiFiS3 handles them
// implicitly) return BB_OK so portable consumer code can call them
// unconditionally. Functions that genuinely don't apply (e.g. async scan)
// are also BB_OK no-ops with a documented behavior — see bb_wifi.h.

#if defined(ARDUINO) && defined(BB_WIFI_BACKEND_R4)

#include "bb_wifi.h"
#include "bb_log.h"
#include "bb_system.h"
#include <Arduino.h>
#include <WiFiS3.h>
#include <string.h>
#include <stdio.h>

extern "C" {

static const char *TAG = "bb_wifi";

// Credentials supplied by the app via -include secrets.h (build_flags).
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// Diagnostic state — synchronously updated from poll_status() since
// Arduino has no event loop equivalent.
static uint8_t  g_disc_reason  = 0;
static unsigned long g_disc_at_ms = 0;
static int      g_retry_count  = 0;
static bool     g_last_status  = false;

static bb_wifi_on_got_ip_cb_t     g_on_got_ip    = NULL;
static bb_wifi_on_disconnect_cb_t g_on_disconnect = NULL;

// Transport state. Single concurrent connection slot — sufficient for
// resource-constrained Arduino targets.
struct bb_conn {
    WiFiClient client;
};
static WiFiServer *g_server = NULL;
static uint16_t    g_listen_port = 0;
static struct bb_conn g_conn_slot;
static bool g_conn_slot_active = false;

static void poll_status(void) {
    bool now = (WiFi.status() == WL_CONNECTED);
    if (!g_last_status && now) {
        g_retry_count = 0;
        bb_log_i(TAG, "got ip %s rssi=%d",
                 WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        if (g_on_got_ip) g_on_got_ip();
    } else if (g_last_status && !now) {
        g_disc_reason = WiFi.status();
        g_disc_at_ms = millis();
        g_retry_count++;
        bb_log_w(TAG, "disconnected status=%d", g_disc_reason);
        if (g_on_disconnect) g_on_disconnect();
    }
    g_last_status = now;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bb_err_t bb_wifi_ensure_netif(void) { return BB_OK; }

bb_err_t bb_wifi_init_sta(void) {
    if (WiFi.status() == WL_NO_MODULE) {
        bb_log_e(TAG, "WiFi module not present");
        return BB_ERR_INVALID_STATE;
    }
    bb_log_i(TAG, "fw %s", WiFi.firmwareVersion());
    bb_log_i(TAG, "connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    while (millis() - start < 30000) {
        delay(1000);
        poll_status();
        if (g_last_status) return BB_OK;
    }
    bb_log_e(TAG, "timeout, status=%d", WiFi.status());
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_wifi_init(void) {
    if (bb_wifi_init_sta() != BB_OK) {
        bb_log_e(TAG, "restarting");
        bb_system_restart();
    }
    return BB_OK;
}

void bb_wifi_force_reassociate(void) {
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ---------------------------------------------------------------------------
// Hostname
// ---------------------------------------------------------------------------

bb_err_t bb_wifi_set_hostname(const char *hostname) {
    if (!hostname || !*hostname) return BB_ERR_INVALID_ARG;
    WiFi.setHostname(hostname);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

int bb_wifi_scan_networks(bb_wifi_ap_t *results, int max_results) {
    if (!results || max_results <= 0) return 0;
    int count = WiFi.scanNetworks();
    if (count <= 0) return 0;
    if (count > max_results) count = max_results;
    for (int i = 0; i < count; i++) {
        memset(&results[i], 0, sizeof(results[i]));
        strncpy(results[i].ssid, WiFi.SSID(i), sizeof(results[i].ssid) - 1);
        results[i].rssi = WiFi.RSSI(i);
        results[i].secure = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
    }
    return count;
}

void bb_wifi_scan_start_async(void) { /* WiFiS3 lacks async scan */ }

int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results) {
    (void)results;
    (void)max_results;
    return 0;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void bb_wifi_register_on_got_ip(bb_wifi_on_got_ip_cb_t cb)         { g_on_got_ip = cb; }
void bb_wifi_register_on_disconnect(bb_wifi_on_disconnect_cb_t cb) { g_on_disconnect = cb; }

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us) {
    if (reason) *reason = g_disc_reason;
    if (age_us) *age_us = g_disc_at_ms ? (int64_t)(millis() - g_disc_at_ms) * 1000 : 0;
}

int bb_wifi_get_retry_count(void) { return g_retry_count; }

bb_err_t bb_wifi_get_ip_str(char *out, size_t out_len) {
    if (!out || out_len < 16) return BB_ERR_INVALID_ARG;
    IPAddress ip = WiFi.localIP();
    snprintf(out, out_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return BB_OK;
}

bb_err_t bb_wifi_get_rssi(int8_t *out) {
    if (!out) return BB_ERR_INVALID_ARG;
    *out = (int8_t)WiFi.RSSI();
    return BB_OK;
}

bool bb_wifi_has_ip(void) { return WiFi.status() == WL_CONNECTED; }

bb_err_t bb_wifi_get_info(bb_wifi_info_t *out) {
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->connected = (WiFi.status() == WL_CONNECTED);
    if (out->connected) {
        strncpy(out->ssid, WiFi.SSID(), sizeof(out->ssid) - 1);
        WiFi.BSSID(out->bssid);
        out->rssi = (int8_t)WiFi.RSSI();
        IPAddress ip = WiFi.localIP();
        snprintf(out->ip, sizeof(out->ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    } else {
        strncpy(out->ip, "0.0.0.0", sizeof(out->ip) - 1);
    }
    out->disc_reason = g_disc_reason;
    out->disc_age_s = g_disc_at_ms ? (millis() - g_disc_at_ms) / 1000 : 0;
    out->retry_count = g_retry_count;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

bb_err_t bb_wifi_listen(uint16_t port) {
    g_listen_port = port;
    return BB_OK;
}

bb_err_t bb_wifi_accept(bb_conn_t **out) {
    if (!out) return BB_ERR_INVALID_ARG;
    *out = NULL;

    poll_status();

    if (!g_server) {
        if (g_listen_port == 0) return BB_ERR_INVALID_STATE;
        g_server = new WiFiServer(g_listen_port);
        g_server->begin();
        bb_log_i(TAG, "listening on port %u", g_listen_port);
    }

    if (g_conn_slot_active) {
        return BB_OK;  // existing connection still holds the slot
    }

    WiFiClient incoming = g_server->available();
    if (!incoming) return BB_OK;

    g_conn_slot.client = incoming;
    g_conn_slot_active = true;
    *out = &g_conn_slot;
    return BB_OK;
}

int bb_conn_available(bb_conn_t *c) {
    return c ? c->client.available() : 0;
}

int bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n) {
    if (!c || !buf) return -1;
    return c->client.read(buf, n);
}

int bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n) {
    if (!c || !buf) return -1;
    return (int)c->client.write(buf, n);
}

void bb_conn_close(bb_conn_t *c) {
    if (!c) return;
    c->client.stop();
    if (c == &g_conn_slot) g_conn_slot_active = false;
}

}  // extern "C"

#endif
