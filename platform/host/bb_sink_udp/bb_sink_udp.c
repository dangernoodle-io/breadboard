// bb_sink_udp — host backend: thin pub-sink adapter. Delegates the actual
// datagram send to bb_udp_client_send() — the host stub there captures
// bytes for tests; see platform/host/bb_udp_client/bb_udp_client.c. The real
// socket path lives in platform/espidf/bb_sink_udp/bb_sink_udp.c (also
// delegating to bb_udp_client) and is exercised via smoke build.
// (KB#702/#710 — split from the transport component bb_udp_client.)
#include "bb_sink_udp.h"
#include "bb_sink_udp_priv.h"
#include "bb_udp_client.h"

static bool s_initialized = false;

bb_err_t bb_sink_udp_init(void)
{
    s_initialized = true;
    return BB_OK;
}

#ifdef BB_SINK_UDP_TESTING
void bb_sink_udp_test_reset(void)
{
    bb_sink_udp_priv_test_reset();
}
#endif /* BB_SINK_UDP_TESTING */

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
