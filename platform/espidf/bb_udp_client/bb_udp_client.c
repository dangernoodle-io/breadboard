// bb_udp_client — ESP-IDF backend: real IPv4 UDP socket transport
// (ouroboros KB#554, extracted per KB#702/#710). One AF_INET/SOCK_DGRAM
// socket, lazily opened on first send and kept open for the process
// lifetime. Unicast sends target cfg.host:cfg.port (literal IPv4, no DNS
// resolution); broadcast mode sends to 255.255.255.255:cfg.port after
// arming SO_BROADCAST once. The destination sockaddr_in is rebuilt from the
// current cfg on every send so a runtime cfg change takes effect
// immediately — the socket itself is never connect()ed. Same-subnet
// delivery needs no gateway hop.
#include "bb_udp_client.h"
#include "bb_udp_client_priv.h"
#include "bb_log.h"

#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "bb_udp_client";

// Single-writer-at-boot invariant: s_cfg / s_sock / s_broadcast_armed /
// s_initialized are plain (non-atomic, unlocked) statics. This is safe
// because init runs exactly once at boot, strictly before any send(); and
// send() runs only on the single caller task. There is no runtime config
// setter that could mutate s_cfg concurrently with a send — so no lock is
// needed. If a runtime reconfigure API is ever added, this invariant must be
// revisited.
static bb_udp_client_cfg_t s_cfg;
static bool                s_initialized     = false;
static int                 s_sock            = -1;
static bool                s_broadcast_armed = false;

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

static bool ensure_socket(void)
{
    if (s_sock >= 0) return true;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        bb_log_e(TAG, "socket() failed");
        return false;
    }
    s_sock            = fd;
    s_broadcast_armed = false;
    return true;
}

static bool ensure_broadcast_enabled(void)
{
    if (s_broadcast_armed) return true;
    int one = 1;
    if (setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0) {
        bb_log_e(TAG, "setsockopt(SO_BROADCAST) failed");
        return false;
    }
    s_broadcast_armed = true;
    return true;
}

bb_err_t bb_udp_client_send(const uint8_t *buf, int len)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    if (!buf || len < 0) return BB_ERR_INVALID_ARG;
    if (!ensure_socket()) return BB_ERR_INVALID_STATE;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(s_cfg.port);

    if (s_cfg.broadcast) {
        if (!ensure_broadcast_enabled()) return BB_ERR_INVALID_STATE;
        dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    } else {
        if (inet_pton(AF_INET, s_cfg.host, &dest.sin_addr) != 1) {
            bb_log_e(TAG, "send: invalid host configured");
            return BB_ERR_INVALID_ARG;
        }
    }

    ssize_t sent = sendto(s_sock, buf, (size_t)len, 0,
                           (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        bb_log_w(TAG, "sendto failed");
        return BB_ERR_INVALID_STATE;
    }
    return BB_OK;
}
