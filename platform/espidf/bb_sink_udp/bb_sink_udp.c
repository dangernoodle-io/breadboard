// bb_sink_udp — ESP-IDF backend: thin pub-sink adapter. Delegates the
// actual datagram send to bb_udp_client_send(), which owns the real
// AF_INET/SOCK_DGRAM socket, destination config, and broadcast arming — see
// platform/espidf/bb_udp_client/bb_udp_client.c (ouroboros KB#554,
// extracted per KB#702/#710).
#include "bb_sink_udp.h"
#include "bb_sink_udp_priv.h"
#include "bb_udp_client.h"
// bb_pub.h comes in transitively via bb_sink_udp.h

static bool s_initialized = false;

bb_err_t bb_sink_udp_init(void)
{
    s_initialized = true;
    return BB_OK;
}

static bb_err_t udp_publish(void *ctx, const char *topic,
                             const char *payload, int len, bool retain)
{
    (void)ctx;
    (void)retain; // documented no-op — UDP has no broker-side retention concept

    if (!s_initialized) return BB_ERR_INVALID_STATE;

    uint8_t buf[BB_SINK_UDP_MTU];
    int n = bb_sink_udp_priv_encode(topic, payload, len, buf, sizeof(buf));
    if (n < 0) return BB_ERR_NO_SPACE; // dropped — logged via priv encode counter

    return bb_udp_client_send(buf, n);
}

bb_err_t bb_sink_udp(bb_pub_sink_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    out->publish       = udp_publish;
    out->ctx           = NULL;
    out->transport     = "udp";
    out->tls           = false;
    out->subscribe     = NULL;
    out->subscribe_ctx = NULL;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// PRE_HTTP-tier init — additive fan-out sink, coexists with bb_sink_mqtt /
// bb_sink_http (NOT the bb_pub exclusive-sink arbiter). Initializes both the
// transport (bb_udp_client, dest config in NVS namespace "bb_udp") and this
// adapter. Called via the bb_app_init() composition root (bbtool:init
// marker in bb_sink_udp.h), not self-registered.
// ---------------------------------------------------------------------------
bb_err_t bb_sink_udp_auto_init(void)
{
    bb_err_t err = bb_udp_client_init(NULL);
    if (err != BB_OK) return err;

    err = bb_sink_udp_init();
    if (err != BB_OK) return err;

    bb_pub_sink_t s;
    err = bb_sink_udp(&s);
    if (err != BB_OK) return err;

    return bb_pub_add_sink(&s);
}
