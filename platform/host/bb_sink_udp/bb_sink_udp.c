// bb_sink_udp — host backend: no real socket. Captures the encoded datagram
// bytes into a small ring so host tests can decode and assert on what would
// have been sent, per ouroboros KB#551 (host-testable seam; the real socket
// path lives in platform/espidf/bb_sink_udp/bb_sink_udp.c and is exercised
// via smoke build).
#include "bb_sink_udp.h"
#include "bb_sink_udp_priv.h"

#include <string.h>

// Single-writer-at-boot invariant: s_cfg / s_initialized are plain
// (non-atomic, unlocked) statics. This is safe because init runs exactly
// once at PRE_HTTP boot, strictly before bb_pub wires up publish; and
// publish() (udp_publish, below) runs only on the single bb_pub worker task
// (or, in host tests, single-threaded). There is no runtime config setter
// that could mutate s_cfg concurrently with a publish — so no lock is
// needed. If a runtime reconfigure API is ever added, this invariant must be
// revisited.
static bb_sink_udp_cfg_t s_cfg;
static bool              s_initialized = false;

bb_err_t bb_sink_udp_init(const bb_sink_udp_cfg_t *cfg_or_null)
{
    if (cfg_or_null) {
        if (strnlen(cfg_or_null->host, sizeof(cfg_or_null->host)) >= sizeof(s_cfg.host)) {
            return BB_ERR_INVALID_ARG;
        }
        s_cfg = *cfg_or_null;
        bb_sink_udp_priv_save_to_nvs(&s_cfg);
    } else {
        bb_sink_udp_priv_load_from_nvs(&s_cfg);
    }
    s_initialized = true;
    return BB_OK;
}

#ifdef BB_SINK_UDP_TESTING
#define BB_SINK_UDP_HOST_CAP_SLOTS 8
static uint8_t s_captured_buf[BB_SINK_UDP_HOST_CAP_SLOTS][BB_SINK_UDP_MTU];
static int     s_captured_len[BB_SINK_UDP_HOST_CAP_SLOTS];
static int     s_captured_count = 0;

int bb_sink_udp_host_capture_count(void)
{
    return s_captured_count;
}

bool bb_sink_udp_host_last_frame(bb_udp_frame_t *out)
{
    if (s_captured_count == 0 || !out) return false;
    int idx = (s_captured_count - 1) % BB_SINK_UDP_HOST_CAP_SLOTS;
    return bb_udp_frame_decode(s_captured_buf[idx], s_captured_len[idx], out) == BB_OK;
}

void bb_sink_udp_test_reset(void)
{
    s_captured_count = 0;
    memset(s_captured_len, 0, sizeof(s_captured_len));
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

#ifdef BB_SINK_UDP_TESTING
    int idx = s_captured_count % BB_SINK_UDP_HOST_CAP_SLOTS;
    memcpy(s_captured_buf[idx], buf, (size_t)n);
    s_captured_len[idx] = n;
    s_captured_count++;
#endif

    return BB_OK; // host stub: "sent" — no real socket
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
