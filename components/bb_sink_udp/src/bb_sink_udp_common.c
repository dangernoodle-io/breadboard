// bb_sink_udp — common logic shared by the host and ESP-IDF backends:
// NVS-backed config load/save and frame encode + dropped-counter bookkeeping.
// Compiled on both platforms; the platform-specific files
// (platform/{host,espidf}/bb_sink_udp/*.c) implement only the actual
// datagram transport (real sendto() vs. host capture buffer).
#include "bb_sink_udp_priv.h"
#include "bb_nv.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BB_SINK_UDP_NVS_NS "bb_sink_udp"

// s_seq and s_dropped are shared file-scope state that will later be reused
// by the paired UDP log sink (bb_sink_udp_log) so both transports share one
// monotonic sequence space and one drop counter — see ouroboros KB#554. Both
// are _Atomic: the eventual second sink writes from a different call path
// (its own publish()), so a plain uint32_t counter would race.
static _Atomic uint32_t s_seq     = 0;
static _Atomic uint32_t s_dropped = 0;

void bb_sink_udp_priv_load_from_nvs(bb_sink_udp_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    bb_nv_get_str(BB_SINK_UDP_NVS_NS, "host", out->host, sizeof(out->host), "");

    char default_port[8];
    snprintf(default_port, sizeof(default_port), "%u", (unsigned)BB_SINK_UDP_PORT);
    char port_str[8];
    bb_nv_get_str(BB_SINK_UDP_NVS_NS, "port", port_str, sizeof(port_str), default_port);
    out->port = (uint16_t)atoi(port_str);

    char bcast_str[4];
    bb_nv_get_str(BB_SINK_UDP_NVS_NS, "broadcast", bcast_str, sizeof(bcast_str),
                  BB_SINK_UDP_BROADCAST ? "1" : "0");
    out->broadcast = (bcast_str[0] == '1');
}

void bb_sink_udp_priv_save_to_nvs(const bb_sink_udp_cfg_t *cfg)
{
    bb_nv_set_str(BB_SINK_UDP_NVS_NS, "host", cfg->host);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)cfg->port);
    bb_nv_set_str(BB_SINK_UDP_NVS_NS, "port", port_str);
    bb_nv_set_str(BB_SINK_UDP_NVS_NS, "broadcast", cfg->broadcast ? "1" : "0");
}

int bb_sink_udp_priv_encode(const char *topic, const char *payload, int len,
                             uint8_t *buf, int buf_cap)
{
    bb_udp_frame_t frame = {
        .kind        = BB_UDP_KIND_TELEMETRY,
        .flags       = 0,
        .seq         = atomic_fetch_add(&s_seq, 1),
        .topic       = topic,
        .topic_len   = topic ? (uint16_t)strlen(topic) : 0,
        .payload     = payload,
        .payload_len = (uint16_t)(len < 0 ? 0 : len),
    };
    int n = bb_udp_frame_encode(&frame, buf, buf_cap);
    if (n < 0) atomic_fetch_add(&s_dropped, 1);
    return n;
}

uint32_t bb_sink_udp_dropped(void)
{
    return atomic_load(&s_dropped);
}

#ifdef BB_SINK_UDP_TESTING
void bb_sink_udp_priv_test_reset(void)
{
    atomic_store(&s_seq, 0);
    atomic_store(&s_dropped, 0);
}
#endif
