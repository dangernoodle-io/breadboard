// bb_sink_udp — IPv4 UDP telemetry egress sink for bb_pub (ouroboros KB#554).
//
// Additive fan-out sink (NOT the bb_pub exclusive-sink arbiter) — coexists
// with bb_sink_mqtt / bb_sink_http. Each publish() call builds ONE UDP
// datagram via bb_udp_frame_encode (kind=BB_UDP_KIND_TELEMETRY) and hands it
// to bb_udp_client_send(), which owns the socket lifecycle and destination
// config (unicast host:port or local broadcast) — see KB#702/#710. This
// component is a thin pub-sink adapter: framing, kind selection, and the
// dropped counter live here; the transport lives in bb_udp_client.
//
// The `topic` bb_pub hands to publish() is already fully-qualified
// ("metrics/<hostname>/<subtopic>") and is forwarded unchanged onto the
// wire — this is what keeps brood transport-transparent across MQTT/HTTP/UDP.
//
// Usage:
//   bb_udp_client_init(NULL);      // NVS-backed dest config (namespace "bb_udp")
//   bb_sink_udp_init();
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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mark the bb_sink_udp adapter ready. Does not configure the transport —
 * callers must separately call bb_udp_client_init() (directly, or via the
 * PRE_HTTP autoregister path on ESP-IDF) before any publish() succeeds.
 *
 * @return BB_OK always.
 */
bb_err_t bb_sink_udp_init(void);

// bbtool:init tier=pre_http fn=bb_sink_udp_auto_init
extern bb_err_t bb_sink_udp_auto_init(void);

/**
 * Fill `out` with a bb_pub_sink_t whose publish() builds one UDP datagram
 * per call (bb_udp_frame_encode + bb_udp_client_send). Sets
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

#endif /* BB_SINK_UDP_TESTING */

#ifdef __cplusplus
}
#endif
