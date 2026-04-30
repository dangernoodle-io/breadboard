// Arduino + Adafruit CC3000 shield bb_wifi backend.
// Selected by -DBB_WIFI_BACKEND_CC3000 in the consumer's build_flags.
//
// Tested target: Arduino UNO R4 Minima + Adafruit CC3000 shield. The same
// backend should work on any Cortex-M Arduino with the shield (electrically
// 3.3V SPI w/ 5V-tolerant inputs).
//
// CC3000 has no scan API and no hostname API in this driver — those calls
// are documented BB_OK / 0 no-ops per bb_wifi.h.

#if defined(ARDUINO) && defined(BB_WIFI_BACKEND_CC3000)

#include "bb_wifi.h"
#include "bb_log.h"
#include "bb_system.h"
#include <Arduino.h>
#include <Adafruit_CC3000.h>
#include <Adafruit_CC3000_Server.h>
#include <SPI.h>
#include <string.h>
#include <stdio.h>

extern "C" {

static const char *TAG = "bb_wifi";

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// Shield pinout — overridable via build_flags. Defaults match Adafruit's
// stock CC3000 shield (v1.0 and v1.1).
#ifndef CC3000_CS
#define CC3000_CS   10
#endif
#ifndef CC3000_IRQ
#define CC3000_IRQ  3
#endif
#ifndef CC3000_VBAT
#define CC3000_VBAT 5
#endif

// File-scope CC3000 instance is fine; only the *server* must defer
// construction until after cc3000.begin() (see bb_wifi_accept).
static Adafruit_CC3000 cc3000(CC3000_CS, CC3000_IRQ, CC3000_VBAT,
                              SPI_CLOCK_DIVIDER);

// Cached connection state.
static bool          g_connected = false;
static uint32_t      g_ip = 0;
static char          g_ssid_cached[33] = {0};

static uint8_t       g_disc_reason = 0;
static unsigned long g_disc_at_ms = 0;
static int           g_retry_count = 0;

static bb_wifi_on_got_ip_cb_t     g_on_got_ip = NULL;
static bb_wifi_on_disconnect_cb_t g_on_disconnect = NULL;

// Transport — server is heap-allocated lazily inside bb_wifi_accept so its
// constructor runs after cc3000.begin() (file-scope construction corrupts
// CC3000 SPI state — verified empirically in earlier bb_http_arduino code).
struct bb_conn {
    Adafruit_CC3000_ClientRef client;
};
static Adafruit_CC3000_Server *g_server = NULL;
static uint16_t                g_listen_port = 0;
static struct bb_conn          g_conn_slot = { Adafruit_CC3000_ClientRef(NULL) };
static bool                    g_conn_slot_active = false;

static bb_err_t connect_internal(void) {
    if (g_connected) return BB_OK;

    bb_log_i(TAG, "cc3000 begin");
    if (!cc3000.begin()) {
        bb_log_e(TAG, "cc3000 begin failed");
        return BB_ERR_INVALID_STATE;
    }

    uint8_t major, minor;
    if (cc3000.getFirmwareVersion(&major, &minor)) {
        bb_log_i(TAG, "fw %u.%u", (unsigned)major, (unsigned)minor);
    }

    if (!cc3000.connectToAP(WIFI_SSID, WIFI_PASS, WLAN_SEC_WPA2)) {
        bb_log_e(TAG, "wifi assoc failed");
        g_disc_at_ms = millis();
        g_retry_count++;
        return BB_ERR_INVALID_STATE;
    }

    unsigned long start = millis();
    while (!cc3000.checkDHCP()) {
        if (millis() - start > 30000) {
            bb_log_e(TAG, "dhcp timeout");
            return BB_ERR_INVALID_STATE;
        }
        delay(100);
    }

    uint32_t nm, gw, dhcp, dns;
    start = millis();
    while (millis() - start < 10000) {
        if (cc3000.getIPAddress(&g_ip, &nm, &gw, &dhcp, &dns) && g_ip != 0) {
            break;
        }
        delay(250);
    }
    if (g_ip == 0) {
        bb_log_e(TAG, "no ip after dhcp");
        return BB_ERR_INVALID_STATE;
    }

    g_connected = true;
    strncpy(g_ssid_cached, WIFI_SSID, sizeof(g_ssid_cached) - 1);
    bb_log_i(TAG, "got ip %lu.%lu.%lu.%lu",
             (unsigned long)((g_ip >> 24) & 0xff),
             (unsigned long)((g_ip >> 16) & 0xff),
             (unsigned long)((g_ip >>  8) & 0xff),
             (unsigned long)( g_ip        & 0xff));
    if (g_on_got_ip) g_on_got_ip();
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bb_err_t bb_wifi_ensure_netif(void) { return BB_OK; }

bb_err_t bb_wifi_init_sta(void) { return connect_internal(); }

bb_err_t bb_wifi_init(void) {
    if (connect_internal() != BB_OK) {
        bb_log_e(TAG, "restarting");
        bb_system_restart();
    }
    return BB_OK;
}

void bb_wifi_force_reassociate(void) { /* CC3000 lacks explicit control */ }

// ---------------------------------------------------------------------------
// Hostname / Scan — not supported by Adafruit_CC3000 driver
// ---------------------------------------------------------------------------

bb_err_t bb_wifi_set_hostname(const char *hostname) {
    (void)hostname;
    return BB_OK;
}

int  bb_wifi_scan_networks(bb_wifi_ap_t *r, int n) { (void)r; (void)n; return 0; }
void bb_wifi_scan_start_async(void) { }
int  bb_wifi_scan_get_cached(bb_wifi_ap_t *r, int n) { (void)r; (void)n; return 0; }

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
    snprintf(out, out_len, "%lu.%lu.%lu.%lu",
             (unsigned long)((g_ip >> 24) & 0xff),
             (unsigned long)((g_ip >> 16) & 0xff),
             (unsigned long)((g_ip >>  8) & 0xff),
             (unsigned long)( g_ip        & 0xff));
    return BB_OK;
}

bb_err_t bb_wifi_get_rssi(int8_t *out) {
    if (!out) return BB_ERR_INVALID_ARG;
    *out = 0;  // CC3000 driver doesn't expose RSSI cleanly
    return BB_OK;
}

bool bb_wifi_has_ip(void) { return g_connected && g_ip != 0; }

bb_err_t bb_wifi_get_info(bb_wifi_info_t *out) {
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->connected = g_connected;
    if (g_connected) {
        strncpy(out->ssid, g_ssid_cached, sizeof(out->ssid) - 1);
        snprintf(out->ip, sizeof(out->ip), "%lu.%lu.%lu.%lu",
                 (unsigned long)((g_ip >> 24) & 0xff),
                 (unsigned long)((g_ip >> 16) & 0xff),
                 (unsigned long)((g_ip >>  8) & 0xff),
                 (unsigned long)( g_ip        & 0xff));
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

    if (!g_connected) return BB_OK;

    if (!g_server) {
        if (g_listen_port == 0) return BB_ERR_INVALID_STATE;
        g_server = new Adafruit_CC3000_Server(g_listen_port);
        g_server->begin();
        bb_log_i(TAG, "listening on port %u", g_listen_port);
    }

    if (g_conn_slot_active) {
        return BB_OK;
    }

    Adafruit_CC3000_ClientRef incoming = g_server->available();
    if (!incoming) return BB_OK;

    g_conn_slot.client = incoming;
    g_conn_slot_active = true;
    *out = &g_conn_slot;
    return BB_OK;
}

int bb_conn_available(bb_conn_t *c) {
    if (!c) return 0;
    return c->client.available();
}

int bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n) {
    if (!c || !buf) return -1;
    int got = 0;
    while ((size_t)got < n && c->client.available()) {
        int b = c->client.read();
        if (b < 0) break;
        buf[got++] = (uint8_t)b;
    }
    return got;
}

int bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n) {
    if (!c || !buf) return -1;
    return (int)c->client.write(buf, n);
}

void bb_conn_close(bb_conn_t *c) {
    if (!c) return;
    c->client.close();
    if (c == &g_conn_slot) g_conn_slot_active = false;
}

}  // extern "C"

#endif
