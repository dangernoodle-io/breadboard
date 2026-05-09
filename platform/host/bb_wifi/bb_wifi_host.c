#include "bb_wifi.h"

#include <string.h>

#ifndef ESP_PLATFORM

// Stub implementations for non-ESP platforms.
// DHCP hostname configuration is not available on host/Arduino.
// Still validates input; network operations return BB_OK (no-op).

bb_err_t bb_wifi_set_hostname(const char *hostname)
{
    if (!hostname || !*hostname) return BB_ERR_INVALID_ARG;
    return BB_OK;
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
    return false;
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

#endif /* !ESP_PLATFORM */
