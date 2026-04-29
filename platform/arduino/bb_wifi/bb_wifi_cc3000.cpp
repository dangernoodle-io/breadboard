#if defined(ARDUINO) && defined(BB_WIFI_BACKEND_CC3000)

#include "bb_wifi.h"
#include "bb_log.h"
#include <Arduino.h>
#include <Adafruit_CC3000.h>
#include <Adafruit_CC3000_Server.h>
#include <string.h>

// Logging tag
static const char *TAG = "bb_wifi";

// CC3000 pinout — defaults to 10/3/5 (Adafruit shield standard)
#ifndef CC3000_CS
#define CC3000_CS 10
#endif
#ifndef CC3000_IRQ
#define CC3000_IRQ 3
#endif
#ifndef CC3000_VBAT
#define CC3000_VBAT 5
#endif

// File-scope Adafruit_CC3000 instance. This is safe to construct at module
// init time because we only call cc3000.begin() from bb_wifi_init[_sta] in
// user code context, not during C++ static initialization.
static Adafruit_CC3000 cc3000(CC3000_CS, CC3000_IRQ, CC3000_VBAT, SPI_CLOCK_DIVIDER);

// Connection state
static bool g_connected = false;
static uint32_t g_ip_addr = 0;
static char g_cached_ssid[33] = {0};

// Retry tracking
static int g_retry_count = 0;
static uint8_t g_disc_reason = 0;
static unsigned long g_disc_at_ms = 0;

// Callbacks
static bb_wifi_on_got_ip_cb_t g_on_got_ip = NULL;
static bb_wifi_on_disconnect_cb_t g_on_disconnect = NULL;

// Transport state
static uint16_t g_listen_port = 0;
static Adafruit_CC3000_Server *g_server = NULL;
static struct {
    Adafruit_CC3000_ClientRef client;
    bool valid;
} g_conn_slot = {nullptr, false};

// Forward declaration
extern "C" {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bb_err_t bb_wifi_ensure_netif(void)
{
    // No-op on Arduino; platform initializes implicitly.
    return BB_OK;
}

static bb_err_t wifi_connect_internal(void)
{
    // Idempotent: return success if already connected with valid IP
    if (g_connected && g_ip_addr != 0) {
        return BB_OK;
    }

    // CC3000 begin
    bb_log_i(TAG, "cc3000 begin...");
    if (!cc3000.begin()) {
        bb_log_e(TAG, "cc3000 begin failed");
        return BB_ERR_INVALID_STATE;
    }
    bb_log_i(TAG, "cc3000 begin ok");

    // Log firmware version
    uint8_t major, minor;
    if (cc3000.getFirmwareVersion(&major, &minor)) {
        bb_log_i(TAG, "fw %u.%u", major, minor);
    }

    // Connect to AP
    if (!cc3000.connectToAP(WIFI_SSID, WIFI_PASS, WLAN_SEC_WPA2)) {
        bb_log_e(TAG, "wifi assoc failed");
        return BB_ERR_INVALID_STATE;
    }

    // Poll DHCP with timeout
    unsigned long timeout = millis() + 30000;  // 30 second timeout
    while (!cc3000.checkDHCP() && millis() < timeout) {
        delay(100);
    }

    if (millis() >= timeout) {
        bb_log_e(TAG, "dhcp timeout");
        return BB_ERR_INVALID_STATE;
    }

    // Poll for IP address (checkDHCP can return true before lease is populated)
    timeout = millis() + 10000;
    uint32_t ip = 0, nm, gw, dhcp, dns;
    while (millis() < timeout) {
        if (cc3000.getIPAddress(&ip, &nm, &gw, &dhcp, &dns) && ip != 0) {
            break;
        }
        delay(250);
    }

    if (ip == 0) {
        bb_log_e(TAG, "no ip address");
        return BB_ERR_INVALID_STATE;
    }

    // Success: cache IP and SSID
    g_ip_addr = ip;
    g_connected = true;
    strncpy(g_cached_ssid, WIFI_SSID, sizeof(g_cached_ssid) - 1);
    g_cached_ssid[sizeof(g_cached_ssid) - 1] = '\0';

    bb_log_i(TAG, "ip %lu.%lu.%lu.%lu",
             (unsigned long)((ip >> 24) & 0xff),
             (unsigned long)((ip >> 16) & 0xff),
             (unsigned long)((ip >> 8) & 0xff),
             (unsigned long)(ip & 0xff));
    bb_log_i(TAG, "online");

    // Fire callback
    if (g_on_got_ip) {
        g_on_got_ip();
    }

    return BB_OK;
}

bb_err_t bb_wifi_init_sta(void)
{
    return wifi_connect_internal();
}

bb_err_t bb_wifi_init(void)
{
    bb_err_t ret = wifi_connect_internal();
    if (ret != BB_OK) {
        bb_log_e(TAG, "init timeout, rebooting");
        bb_system_restart();
    }
    return ret;
}

void bb_wifi_force_reassociate(void)
{
    // No-op on Arduino; CC3000 lacks explicit reassociation control.
}

// ---------------------------------------------------------------------------
// Hostname (not supported on CC3000)
// ---------------------------------------------------------------------------

bb_err_t bb_wifi_set_hostname(const char *hostname)
{
    (void)hostname;
    // CC3000 has no hostname API.
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Scan (not supported on CC3000)
// ---------------------------------------------------------------------------

int bb_wifi_scan_networks(bb_wifi_ap_t *results, int max_results)
{
    (void)results;
    (void)max_results;
    // CC3000 scan API is not exposed in Adafruit library; not implemented.
    return 0;
}

void bb_wifi_scan_start_async(void)
{
    // No-op on CC3000.
}

int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results)
{
    (void)results;
    (void)max_results;
    // No cached scan on CC3000.
    return 0;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void bb_wifi_register_on_got_ip(bb_wifi_on_got_ip_cb_t cb)
{
    g_on_got_ip = cb;
}

void bb_wifi_register_on_disconnect(bb_wifi_on_disconnect_cb_t cb)
{
    g_on_disconnect = cb;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us)
{
    if (reason) *reason = g_disc_reason;
    if (age_us) {
        if (g_disc_at_ms == 0) {
            *age_us = 0;
        } else {
            unsigned long elapsed_ms = millis() - g_disc_at_ms;
            *age_us = (int64_t)elapsed_ms * 1000LL;
        }
    }
}

int bb_wifi_get_retry_count(void)
{
    return g_retry_count;
}

bb_err_t bb_wifi_get_ip_str(char *out, size_t out_len)
{
    if (!out || out_len < 7) {
        return BB_ERR_INVALID_ARG;
    }

    if (g_ip_addr == 0) {
        strncpy(out, "0.0.0.0", out_len - 1);
        out[out_len - 1] = '\0';
        return BB_OK;
    }

    snprintf(out, out_len, "%lu.%lu.%lu.%lu",
             (unsigned long)((g_ip_addr >> 24) & 0xff),
             (unsigned long)((g_ip_addr >> 16) & 0xff),
             (unsigned long)((g_ip_addr >> 8) & 0xff),
             (unsigned long)(g_ip_addr & 0xff));
    return BB_OK;
}

bb_err_t bb_wifi_get_rssi(int8_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }

    // CC3000's getInfo doesn't reliably expose RSSI; return 0 (no signal).
    *out = 0;
    return BB_OK;
}

bool bb_wifi_has_ip(void)
{
    return g_connected && g_ip_addr != 0;
}

bb_err_t bb_wifi_get_info(bb_wifi_info_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (g_connected && g_ip_addr != 0) {
        out->connected = true;
        strncpy(out->ssid, g_cached_ssid, sizeof(out->ssid) - 1);
        out->ssid[sizeof(out->ssid) - 1] = '\0';

        snprintf(out->ip, sizeof(out->ip), "%lu.%lu.%lu.%lu",
                 (unsigned long)((g_ip_addr >> 24) & 0xff),
                 (unsigned long)((g_ip_addr >> 16) & 0xff),
                 (unsigned long)((g_ip_addr >> 8) & 0xff),
                 (unsigned long)(g_ip_addr & 0xff));

        out->rssi = 0;  // CC3000 doesn't expose RSSI reliably
        // BSSID and other fields remain zeroed
    }

    out->disc_reason = g_disc_reason;
    out->disc_age_s = (g_disc_at_ms == 0) ? 0 : (millis() - g_disc_at_ms) / 1000;
    out->retry_count = g_retry_count;

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Transport (bb_wifi_listen / bb_wifi_accept / bb_conn_*)
// ---------------------------------------------------------------------------

typedef struct bb_conn {
    Adafruit_CC3000_ClientRef client;
} bb_conn_t;

bb_err_t bb_wifi_listen(uint16_t port)
{
    if (g_listen_port != 0 && g_listen_port == port) {
        return BB_OK;  // Already listening
    }

    g_listen_port = port;
    // Server is deferred until first bb_wifi_accept to avoid corrupting CC3000
    // SPI state before cc3000.begin().
    return BB_OK;
}

bb_err_t bb_wifi_accept(bb_conn_t **out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }

    *out = NULL;

    // Deferred server initialization: construct and begin on first call
    if (g_server == NULL && g_listen_port != 0) {
        g_server = new Adafruit_CC3000_Server(g_listen_port);
        if (g_server) {
            g_server->begin();
            bb_log_i(TAG, "listening on :%u", g_listen_port);
        }
    }

    if (g_server == NULL) {
        return BB_ERR_INVALID_STATE;
    }

    // Try to accept a connection (non-blocking)
    Adafruit_CC3000_ClientRef client = g_server->available();
    if (!client) {
        return BB_OK;  // No connection pending
    }

    // Store in static slot and return opaque handle
    g_conn_slot.client = client;
    g_conn_slot.valid = true;
    *out = (bb_conn_t *)&g_conn_slot;

    return BB_OK;
}

int bb_conn_available(bb_conn_t *c)
{
    if (!c || c != (bb_conn_t *)&g_conn_slot || !g_conn_slot.valid) {
        return 0;
    }

    return g_conn_slot.client.available();
}

int bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n)
{
    if (!c || !buf || n == 0) {
        return -1;
    }

    if (c != (bb_conn_t *)&g_conn_slot || !g_conn_slot.valid) {
        return -1;
    }

    int avail = g_conn_slot.client.available();
    if (avail <= 0) {
        return 0;
    }

    size_t to_read = (size_t)avail > n ? n : (size_t)avail;
    int nread = 0;

    for (size_t i = 0; i < to_read; i++) {
        int byte = g_conn_slot.client.read();
        if (byte < 0) {
            break;
        }
        buf[i] = (uint8_t)byte;
        nread++;
    }

    return nread;
}

int bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n)
{
    if (!c || !buf || n == 0) {
        return -1;
    }

    if (c != (bb_conn_t *)&g_conn_slot || !g_conn_slot.valid) {
        return -1;
    }

    size_t nwritten = g_conn_slot.client.write(buf, n);
    return (int)nwritten;
}

void bb_conn_close(bb_conn_t *c)
{
    if (!c || c != (bb_conn_t *)&g_conn_slot || !g_conn_slot.valid) {
        return;
    }

    g_conn_slot.client.close();
    g_conn_slot.valid = false;
}

}  // extern "C"

#endif  // defined(ARDUINO) && defined(BB_WIFI_BACKEND_CC3000)
