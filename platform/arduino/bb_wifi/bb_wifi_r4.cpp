#if defined(ARDUINO) && defined(BB_WIFI_BACKEND_R4)

#include "bb_wifi.h"
#include "bb_log.h"
#include "bb_system.h"
#include <WiFiS3.h>
#include <string.h>

extern "C" {

static const char *TAG = "bb_wifi";

// ============================================================================
// Credential macros — provided by app via secrets.h or similar
// ============================================================================
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// ============================================================================
// State tracking
// ============================================================================
static uint8_t g_disc_reason = 0;
static unsigned long g_disc_at_ms = 0;
static int g_retry_count = 0;
static bool g_ever_connected = false;
static bool g_last_status = false;  // true if WL_CONNECTED, false otherwise

// Callbacks
static bb_wifi_on_got_ip_cb_t g_on_got_ip_cb = NULL;
static bb_wifi_on_disconnect_cb_t g_on_disconnect_cb = NULL;

// Transport
struct bb_conn {
    WiFiClient client;
};

static WiFiServer *g_server = NULL;
static struct bb_conn g_conn_slot;
static bool g_conn_slot_active = false;

// ============================================================================
// Internal helpers
// ============================================================================

static void poll_status(void)
{
    uint8_t status = WiFi.status();
    bool now_connected = (status == WL_CONNECTED);

    // Transition: not-connected → connected
    if (!g_last_status && now_connected) {
        g_retry_count = 0;
        g_ever_connected = true;
        bb_log_i(TAG, "connected to %s", WiFi.SSID());
        if (g_on_got_ip_cb) {
            g_on_got_ip_cb();
        }
    }
    // Transition: connected → not-connected
    else if (g_last_status && !now_connected) {
        g_disc_reason = status;  // WiFi.status() code
        g_disc_at_ms = millis();
        g_retry_count++;
        bb_log_w(TAG, "disconnected (status=%d, retry_count=%d)", status, g_retry_count);
        if (g_on_disconnect_cb) {
            g_on_disconnect_cb();
        }
    }

    g_last_status = now_connected;
}

// ============================================================================
// Lifecycle
// ============================================================================

bb_err_t bb_wifi_ensure_netif(void)
{
    // Arduino WiFi stack is always initialized implicitly; no-op.
    return BB_OK;
}

bb_err_t bb_wifi_init_sta(void)
{
    bb_log_i(TAG, "connecting to %s", WIFI_SSID);

    // Begin association
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Poll for 30 seconds
    unsigned long start_ms = millis();
    const unsigned long timeout_ms = 30000;
    const unsigned long poll_interval_ms = 500;

    while (millis() - start_ms < timeout_ms) {
        delay(poll_interval_ms);
        poll_status();

        if (g_last_status) {
            bb_log_i(TAG, "connected (IP: %s)", WiFi.localIP().toString().c_str());
            return BB_OK;
        }
    }

    bb_log_e(TAG, "timeout connecting to WiFi after 30s");
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_wifi_init(void)
{
    bb_err_t err = bb_wifi_init_sta();
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to connect, restarting");
        bb_system_restart();
    }
    return err;
}

void bb_wifi_force_reassociate(void)
{
    bb_log_w(TAG, "forcing WiFi reassociation");
    WiFi.disconnect(true);  // turn off radio
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ============================================================================
// Hostname
// ============================================================================

bb_err_t bb_wifi_set_hostname(const char *hostname)
{
    if (!hostname || !*hostname) {
        return BB_ERR_INVALID_ARG;
    }

    WiFi.setHostname(hostname);
    return BB_OK;
}

// ============================================================================
// Scan
// ============================================================================

int bb_wifi_scan_networks(bb_wifi_ap_t *results, int max_results)
{
    if (!results || max_results <= 0) {
        return 0;
    }

    int count = WiFi.scanNetworks();
    if (count <= 0) {
        return 0;
    }

    if (count > max_results) {
        count = max_results;
    }

    for (int i = 0; i < count; i++) {
        memset(results[i].ssid, 0, sizeof(results[i].ssid));
        strncpy(results[i].ssid, WiFi.SSID(i), sizeof(results[i].ssid) - 1);
        results[i].rssi = WiFi.RSSI(i);
        results[i].secure = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
    }

    WiFi.scanDelete();
    return count;
}

void bb_wifi_scan_start_async(void)
{
    // Arduino WiFiS3 doesn't support async scan; no-op.
}

int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results)
{
    // No cache on Arduino; return 0.
    (void)results;
    (void)max_results;
    return 0;
}

// ============================================================================
// Callbacks
// ============================================================================

void bb_wifi_register_on_got_ip(bb_wifi_on_got_ip_cb_t cb)
{
    g_on_got_ip_cb = cb;

    // If already connected, fire immediately
    if (cb && g_last_status) {
        cb();
    }
}

void bb_wifi_register_on_disconnect(bb_wifi_on_disconnect_cb_t cb)
{
    g_on_disconnect_cb = cb;
}

// ============================================================================
// Diagnostics
// ============================================================================

void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us)
{
    if (reason) {
        *reason = g_disc_reason;
    }
    if (age_us) {
        if (g_ever_connected && g_disc_at_ms > 0 && !g_last_status) {
            unsigned long age_ms = millis() - g_disc_at_ms;
            *age_us = (int64_t)age_ms * 1000;
        } else {
            *age_us = 0;
        }
    }
}

int bb_wifi_get_retry_count(void)
{
    return g_retry_count;
}

bb_err_t bb_wifi_get_ip_str(char *out, size_t out_len)
{
    if (!out || out_len < 16) {
        return BB_ERR_INVALID_ARG;
    }

    if (g_last_status) {
        IPAddress ip = WiFi.localIP();
        snprintf(out, out_len, "%u.%u.%u.%u",
                 ip[0], ip[1], ip[2], ip[3]);
    } else {
        snprintf(out, out_len, "0.0.0.0");
    }

    return BB_OK;
}

bb_err_t bb_wifi_get_rssi(int8_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }

    if (g_last_status) {
        *out = WiFi.RSSI();
    } else {
        *out = 0;
    }

    return BB_OK;
}

bool bb_wifi_has_ip(void)
{
    return g_last_status;
}

bb_err_t bb_wifi_get_info(bb_wifi_info_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (g_last_status) {
        strncpy(out->ssid, WiFi.SSID(), sizeof(out->ssid) - 1);
        out->ssid[sizeof(out->ssid) - 1] = '\0';

        uint8_t bssid[6];
        WiFi.BSSID(bssid);
        memcpy(out->bssid, bssid, sizeof(out->bssid));

        out->rssi = WiFi.RSSI();

        IPAddress ip = WiFi.localIP();
        snprintf(out->ip, sizeof(out->ip), "%u.%u.%u.%u",
                 ip[0], ip[1], ip[2], ip[3]);

        out->connected = true;
    } else {
        snprintf(out->ip, sizeof(out->ip), "0.0.0.0");
        out->connected = false;
    }

    out->disc_reason = g_disc_reason;

    if (g_ever_connected && g_disc_at_ms > 0 && !g_last_status) {
        unsigned long age_ms = millis() - g_disc_at_ms;
        out->disc_age_s = (uint32_t)(age_ms / 1000);
    } else {
        out->disc_age_s = 0;
    }

    out->retry_count = g_retry_count;

    return BB_OK;
}

// ============================================================================
// Transport
// ============================================================================

bb_err_t bb_wifi_listen(uint16_t port)
{
    if (!g_server) {
        g_server = new WiFiServer(port);
    }

    g_server->begin();
    bb_log_i(TAG, "listening on port %u", port);

    return BB_OK;
}

bb_err_t bb_wifi_accept(bb_conn_t **out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }

    // Poll status to catch mid-poll disconnects
    poll_status();

    if (!g_server) {
        *out = NULL;
        return BB_OK;
    }

    // Try to accept a new client
    WiFiClient client = g_server->available();
    if (!client) {
        *out = NULL;
        return BB_OK;
    }

    // Store in the static slot
    g_conn_slot.client = client;
    g_conn_slot_active = true;
    *out = &g_conn_slot;

    bb_log_d(TAG, "accepted connection from %s:%u",
             client.remoteIP().toString().c_str(),
             client.remotePort());

    return BB_OK;
}

int bb_conn_available(bb_conn_t *c)
{
    if (!c || !g_conn_slot_active) {
        return 0;
    }

    return c->client.available();
}

int bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n)
{
    if (!c || !buf || n == 0 || !g_conn_slot_active) {
        return -1;
    }

    if (!c->client.connected()) {
        g_conn_slot_active = false;
        return -1;
    }

    int bytes_read = c->client.read(buf, n);
    return (bytes_read > 0) ? bytes_read : 0;
}

int bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n)
{
    if (!c || !buf || n == 0 || !g_conn_slot_active) {
        return -1;
    }

    if (!c->client.connected()) {
        g_conn_slot_active = false;
        return -1;
    }

    size_t written = c->client.write(buf, n);
    return (written > 0) ? (int)written : 0;
}

void bb_conn_close(bb_conn_t *c)
{
    if (!c || !g_conn_slot_active) {
        return;
    }

    if (c->client.connected()) {
        c->client.stop();
    }

    g_conn_slot_active = false;
    bb_log_d(TAG, "connection closed");
}

}  // extern "C"

#endif  // defined(ARDUINO) && defined(BB_WIFI_BACKEND_R4)
