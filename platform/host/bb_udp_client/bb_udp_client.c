// bb_udp_client — host backend: no real socket. Captures the raw bytes
// handed to bb_udp_client_send() into a small ring so host tests can assert
// on what would have been sent, per ouroboros KB#551 (host-testable seam;
// the real socket path lives in platform/espidf/bb_udp_client/bb_udp_client.c
// and is exercised via smoke build). This capture buffer has no framing
// knowledge; it just stores whatever bytes the caller sent.
#include "bb_udp_client.h"
#include "bb_udp_client_priv.h"

#include <string.h>

// Single-writer-at-boot invariant: s_cfg / s_initialized are plain
// (non-atomic, unlocked) statics. This is safe because init runs exactly
// once at boot, strictly before any send(); and send() runs only on the
// single caller task (or, in host tests, single-threaded). There is no
// runtime config setter that could mutate s_cfg concurrently with a send —
// so no lock is needed. If a runtime reconfigure API is ever added, this
// invariant must be revisited.
static bb_udp_client_cfg_t s_cfg;
static bool                s_initialized = false;

bb_err_t bb_udp_client_init(const bb_udp_client_cfg_t *cfg_or_null)
{
    if (cfg_or_null) {
        if (strnlen(cfg_or_null->host, sizeof(cfg_or_null->host)) >= sizeof(s_cfg.host)) {
            return BB_ERR_INVALID_ARG;
        }
        s_cfg = *cfg_or_null;
        bb_udp_client_priv_save_to_nvs(&s_cfg);
    } else {
        bb_udp_client_priv_load_from_nvs(&s_cfg);
    }
    s_initialized = true;
    return BB_OK;
}

#ifdef BB_UDP_CLIENT_TESTING
#define BB_UDP_CLIENT_CAP_SLOTS    8
#define BB_UDP_CLIENT_CAP_BUF_BYTES 2048
static uint8_t s_captured_buf[BB_UDP_CLIENT_CAP_SLOTS][BB_UDP_CLIENT_CAP_BUF_BYTES];
static int     s_captured_len[BB_UDP_CLIENT_CAP_SLOTS];
static int     s_captured_count = 0;

int bb_udp_client_host_capture_count(void)
{
    return s_captured_count;
}

int bb_udp_client_host_last_capture(uint8_t *out, int out_cap)
{
    if (s_captured_count == 0 || !out) return -1;
    int idx = (s_captured_count - 1) % BB_UDP_CLIENT_CAP_SLOTS;
    int n = s_captured_len[idx];
    if (n > out_cap) return -1;
    memcpy(out, s_captured_buf[idx], (size_t)n);
    return n;
}

void bb_udp_client_test_reset(void)
{
    s_captured_count = 0;
    memset(s_captured_len, 0, sizeof(s_captured_len));
}
#endif /* BB_UDP_CLIENT_TESTING */

bb_err_t bb_udp_client_send(const uint8_t *buf, int len)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    if (!buf || len < 0) return BB_ERR_INVALID_ARG;

#ifdef BB_UDP_CLIENT_TESTING
    if (len > BB_UDP_CLIENT_CAP_BUF_BYTES) return BB_ERR_NO_SPACE;
    int idx = s_captured_count % BB_UDP_CLIENT_CAP_SLOTS;
    memcpy(s_captured_buf[idx], buf, (size_t)len);
    s_captured_len[idx] = len;
    s_captured_count++;
#endif

    return BB_OK; // host stub: "sent" — no real socket
}
