// bb_sink_udp — IPv4 UDP telemetry egress sink for bb_pub (ouroboros KB#554).
//
// Additive fan-out sink (NOT the bb_pub exclusive-sink arbiter) — coexists
// with bb_sink_mqtt / bb_sink_http. Each publish() call builds ONE UDP
// datagram via bb_udp_frame_encode (kind=BB_UDP_KIND_TELEMETRY) and sends it
// to a configured unicast host:port or the local broadcast address. Designed
// for same-subnet delivery — no gateway hop required.
//
// The `topic` bb_pub hands to publish() is already fully-qualified
// ("metrics/<hostname>/<subtopic>") and is forwarded unchanged onto the
// wire — this is what keeps brood transport-transparent across MQTT/HTTP/UDP.
//
// Usage:
//   bb_sink_udp_init(NULL);        // NVS-backed config (namespace "bb_sink_udp")
//   bb_pub_sink_t s;
//   bb_sink_udp(&s);
//   bb_pub_add_sink(&s);
//
// `retain` is accepted for bb_pub_sink_t API compatibility but is a
// documented no-op — UDP has no broker-side retention concept.
#pragma once

#include "bb_core.h"
#include "bb_pub.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef BB_SINK_UDP_TESTING
#include "bb_udp_frame.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BB_SINK_UDP_HOST_MAX 64

typedef struct {
    char     host[BB_SINK_UDP_HOST_MAX]; /**< Literal IPv4 dotted-quad; no DNS resolution. */
    uint16_t port;
    bool     broadcast; /**< true = send to 255.255.255.255:port instead of host. */
} bb_sink_udp_cfg_t;

/**
 * Initialize bb_sink_udp config. NVS-backed (namespace "bb_sink_udp").
 * Pass NULL to load the persisted config (falling back to Kconfig defaults
 * when NVS is empty); pass non-NULL to override and persist `cfg`.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if cfg is non-NULL and
 *         cfg->host does not fit in BB_SINK_UDP_HOST_MAX - 1 chars.
 */
bb_err_t bb_sink_udp_init(const bb_sink_udp_cfg_t *cfg_or_null);

/**
 * Fill `out` with a bb_pub_sink_t whose publish() builds and sends one UDP
 * datagram per call (bb_udp_frame_encode + platform transport). Sets
 * out->transport = "udp"; out->tls is always false (UDP has no TLS mode).
 * Must be called after bb_sink_udp_init.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if out is NULL;
 *         BB_ERR_INVALID_STATE if bb_sink_udp_init has not been called.
 */
bb_err_t bb_sink_udp(bb_pub_sink_t *out);

/**
 * Count of publishes dropped because the encoded frame (header + topic +
 * payload) exceeded CONFIG_BB_SINK_UDP_MTU. bb_sink_udp never fragments —
 * oversized datagrams are dropped, not truncated or split across packets.
 */
uint32_t bb_sink_udp_dropped(void);

// ---------------------------------------------------------------------------
// Host test hooks (only when BB_SINK_UDP_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_SINK_UDP_TESTING

/** Reset seq counter and dropped counter (test isolation). */
void bb_sink_udp_test_reset(void);

/**
 * Number of datagrams captured by the host stub since the last
 * bb_sink_udp_test_reset() (or process start).
 */
int bb_sink_udp_host_capture_count(void);

/**
 * Decode the most recently captured datagram into `out`.
 * `out->topic`/`out->payload` point into internal storage — valid until the
 * next publish() call. Returns false if no datagram has been captured yet
 * or the captured bytes fail to decode.
 */
bool bb_sink_udp_host_last_frame(bb_udp_frame_t *out);

#endif /* BB_SINK_UDP_TESTING */

#ifdef __cplusplus
}
#endif
