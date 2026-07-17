// Arduino stub for bb_tcp_client — returns BB_ERR_UNSUPPORTED. Arduino
// targets in this workspace are AVR / Cortex-M class with no shared
// connected-TCP-stream primitive; implement per-board (WiFiClient) if a real
// consumer appears. Mirrors platform/arduino/bb_http_client's stub
// precedent.
#include "bb_tcp_client.h"

#include <string.h>

bb_err_t bb_tcp_client_init(const char *ns, const bb_tcp_client_cfg_t *cfg_or_null, bb_tcp_client_t *out)
{
    (void)ns; (void)cfg_or_null;
    if (out) *out = NULL;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_tcp_client_connect(bb_tcp_client_t h)
{
    (void)h;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_tcp_client_read(bb_tcp_client_t h, uint8_t *buf, size_t len, size_t *out_len)
{
    (void)h; (void)buf; (void)len;
    if (out_len) *out_len = 0;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_tcp_client_write(bb_tcp_client_t h, const uint8_t *buf, size_t len)
{
    (void)h; (void)buf; (void)len;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_tcp_client_poll_readable(bb_tcp_client_t h, uint32_t timeout_ms, bool *out_readable)
{
    (void)h; (void)timeout_ms;
    if (out_readable) *out_readable = false;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_tcp_client_close(bb_tcp_client_t h)
{
    (void)h;
    return BB_ERR_UNSUPPORTED;
}

bb_tcp_client_state_t bb_tcp_client_get_state(bb_tcp_client_t h)
{
    (void)h;
    return BB_TCP_CLIENT_DISCONNECTED;
}

bb_err_t bb_tcp_client_destroy(bb_tcp_client_t h)
{
    (void)h;
    return BB_OK;  // NULL/no-op-safe, mirrors every other backend's destroy contract
}

bb_err_t bb_tcp_client_health_fill(bb_tcp_client_t h, bb_tcp_client_health_snap_t *out)
{
    (void)h;
    if (out) memset(out, 0, sizeof(*out));
    return BB_ERR_UNSUPPORTED;
}
