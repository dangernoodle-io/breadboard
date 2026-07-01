#include "bb_wifi.h"

#include <string.h>

#ifndef ESP_PLATFORM

// Stub implementations for non-ESP platforms.
// DHCP hostname configuration is not available on host/Arduino.
// Still validates input; network operations return BB_OK (no-op).

#ifdef BB_WIFI_TESTING
#include "bb_wifi_test.h"
static bool s_test_has_ip = false;
static int s_test_recovery_count = 0;
static const char *s_test_last_recovery_reason = NULL;
static bool s_test_recovery_blocked = false;
static uint32_t s_test_restart_sta_count = 0;
static int8_t s_test_disconnect_rssi = 0;

void bb_wifi_test_set_has_ip(bool has_ip)
{
    s_test_has_ip = has_ip;
}

void bb_wifi_test_set_recovery_blocked(bool blocked)
{
    s_test_recovery_blocked = blocked;
}

int bb_wifi_test_get_recovery_count(void)
{
    return s_test_recovery_count;
}

const char *bb_wifi_test_get_last_recovery_reason(void)
{
    return s_test_last_recovery_reason;
}

void bb_wifi_test_reset_recovery(void)
{
    s_test_recovery_count = 0;
    s_test_last_recovery_reason = NULL;
    s_test_recovery_blocked = false;
}

void bb_wifi_test_set_restart_sta_count(uint32_t count)
{
    s_test_restart_sta_count = count;
}

void bb_wifi_test_set_disconnect_rssi(int8_t rssi)
{
    s_test_disconnect_rssi = rssi;
}
#endif /* BB_WIFI_TESTING */

bb_err_t bb_wifi_set_hostname(const char *hostname)
{
    if (!hostname || !*hostname) return BB_ERR_INVALID_ARG;
    return BB_OK;
}

bb_err_t bb_wifi_reconfigure(const char *ssid, const char *pass)
{
    (void)ssid;
    (void)pass;
    return BB_ERR_UNSUPPORTED;
}

// Return a zeroed-out info snapshot. No network state is available on host.
bb_err_t bb_wifi_get_info(bb_wifi_info_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    // Set "0.0.0.0" for ip — same as disconnected device
    strncpy(out->ip, "0.0.0.0", sizeof(out->ip) - 1);
    return BB_OK;
}

void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us)
{
    if (reason) *reason = 0;
    if (age_us) *age_us = 0;
}

int bb_wifi_get_retry_count(void)
{
    return 0;
}

bool bb_wifi_has_ip(void)
{
#ifdef BB_WIFI_TESTING
    return s_test_has_ip;
#else
    return false;
#endif
}

bb_err_t bb_wifi_get_ip_str(char *out, size_t out_len)
{
    if (!out || out_len == 0) return BB_ERR_INVALID_ARG;
    strncpy(out, "0.0.0.0", out_len - 1);
    out[out_len - 1] = '\0';
    return BB_OK;
}

bb_err_t bb_wifi_get_rssi(int8_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    *out = 0;
    return BB_OK;
}

void bb_wifi_restart_sta(void) {}

void bb_wifi_scan_start_async(void) {}

int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results)
{
    (void)results;
    (void)max_results;
    return 0;
}

bb_err_t bb_wifi_listen(uint16_t port)
{
    (void)port;
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_wifi_accept(bb_conn_t **out)
{
    (void)out;
    return BB_ERR_INVALID_STATE;
}

int bb_conn_available(bb_conn_t *c)
{
    (void)c;
    return 0;
}

int bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n)
{
    (void)c;
    (void)buf;
    (void)n;
    return -1;
}

int bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n)
{
    (void)c;
    (void)buf;
    (void)n;
    return -1;
}

void bb_conn_close(bb_conn_t *c)
{
    (void)c;
}

uint32_t bb_wifi_get_lost_ip_count(void)
{
    return 0;
}

uint32_t bb_wifi_get_egress_dead_count(void)
{
    return 0;
}

uint32_t bb_wifi_get_no_ip_count(void)
{
    return 0;
}

uint32_t bb_wifi_get_restart_sta_count(void)
{
#ifdef BB_WIFI_TESTING
    return s_test_restart_sta_count;
#else
    return 0;
#endif
}

int8_t bb_wifi_get_disconnect_rssi(void)
{
#ifdef BB_WIFI_TESTING
    return s_test_disconnect_rssi;
#else
    return 0;
#endif
}

void bb_wifi_get_reason_histogram(uint16_t *out, size_t len)
{
    if (!out || len == 0) return;
    memset(out, 0, len * sizeof(uint16_t));
}

#ifdef BB_WIFI_TESTING
static bool s_test_gateway_reachable = true;

void bb_wifi_host_set_gateway_reachable(bool reachable)
{
    s_test_gateway_reachable = reachable;
}
#endif /* BB_WIFI_TESTING */

bb_err_t bb_wifi_request_recovery(const char *reason)
{
#ifdef BB_WIFI_TESTING
    if (!s_test_has_ip) {
        return BB_OK; // no-op: no IP, FSM owns recovery
    }
    if (!s_test_recovery_blocked) {
        s_test_recovery_count++;
        s_test_last_recovery_reason = reason;
    }
    return BB_OK;
#else
    (void)reason;
    return BB_OK;
#endif
}

#endif /* !ESP_PLATFORM */
