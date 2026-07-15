// bb_udp_client — reusable IPv4 UDP datagram transport.
//
/**
 * @brief Reusable IPv4 UDP datagram transport (ouroboros KB#702/#710) — the
 * datagram peer to bb_tcp_client.
 */
//
// Owns the socket lifecycle, destination config (unicast host:port or local
// broadcast), and the actual send. Framing/encoding is NOT this component's
// concern — callers hand it pre-encoded bytes (e.g. a bb_udp_frame) and call
// bb_udp_client_send().
//
// Usage:
//   bb_udp_client_cfg_t cfg = { .host = "192.168.1.50", .port = 9109 };
//   bb_udp_client_init("my_ns", &cfg);  // or NULL cfg to load NVS-backed / Kconfig config
//   bb_udp_client_send(buf, len);
#pragma once

#include "bb_core.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_UDP_CLIENT_HOST_MAX 64

typedef struct {
    char     host[BB_UDP_CLIENT_HOST_MAX]; /**< Literal IPv4 dotted-quad; no DNS resolution. */
    uint16_t port;
    bool     broadcast; /**< true = send to 255.255.255.255:port instead of host. */
} bb_udp_client_cfg_t;

/**
 * Initialize bb_udp_client config. NVS-backed (namespace `ns`).
 * Pass cfg_or_null = NULL to load the persisted config (NVS namespace `ns`),
 * falling back to Kconfig defaults when NVS is empty; pass non-NULL to
 * override host/port/broadcast and persist them under `ns`.
 *
 * @param ns           NVS namespace to load/persist host/port/broadcast
 *                      under. Required, borrowed (not copied — used only
 *                      for the duration of this call), ≤15 chars (NVS
 *                      namespace limit). NULL or empty ⇒ BB_ERR_INVALID_ARG
 *                      (before any storage is touched). The caller/
 *                      composition decides WHERE config lives; this
 *                      component only declares WHAT it stores.
 * @param cfg_or_null  Configuration, or NULL to load persisted/Kconfig defaults.
 * @return BB_OK on success; BB_ERR_INVALID_ARG if ns is NULL/empty, or
 *         cfg_or_null is non-NULL and cfg_or_null->host does not fit in
 *         BB_UDP_CLIENT_HOST_MAX - 1 chars.
 */
bb_err_t bb_udp_client_init(const char *ns, const bb_udp_client_cfg_t *cfg_or_null);

/**
 * Send `len` bytes of `buf` as one UDP datagram to the configured
 * destination (unicast host:port, or local broadcast when cfg.broadcast is
 * set). The destination sockaddr is rebuilt from the current cfg on every
 * call, so a runtime cfg change takes effect immediately.
 *
 * @return BB_OK on success; BB_ERR_INVALID_STATE if bb_udp_client_init has
 *         not been called, the socket could not be opened/configured, or the
 *         underlying send fails; BB_ERR_INVALID_ARG if the configured host
 *         is not a valid dotted-quad (unicast mode only).
 */
bb_err_t bb_udp_client_send(const uint8_t *buf, int len);

// ---------------------------------------------------------------------------
// Host test hooks (only when BB_UDP_CLIENT_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_UDP_CLIENT_TESTING

/** Reset the host capture buffer (test isolation). */
void bb_udp_client_test_reset(void);

/**
 * Number of datagrams captured by the host stub since the last
 * bb_udp_client_test_reset() (or process start).
 */
int bb_udp_client_host_capture_count(void);

/**
 * Copy the most recently captured datagram's raw bytes into `out` (capacity
 * `out_cap`). Returns the captured length, or -1 if no datagram has been
 * captured yet or it does not fit in `out_cap`.
 */
int bb_udp_client_host_last_capture(uint8_t *out, int out_cap);

#endif /* BB_UDP_CLIENT_TESTING */

#ifdef __cplusplus
}
#endif
