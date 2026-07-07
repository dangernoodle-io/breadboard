// bb_udp_client — common logic shared by the host and ESP-IDF backends:
// NVS-backed dest-config load/save. The platform-specific files
// (platform/{host,espidf}/bb_udp_client/*.c) implement init state and the
// actual datagram transport (real sendto() vs. host capture buffer).
#include "bb_udp_client_priv.h"
#include "bb_nv.h"
#include "bb_nv_namespaces.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bb_udp_client_priv_load_from_nvs(bb_udp_client_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    bb_nv_get_str(BB_UDP_NVS_NS, "host", out->host, sizeof(out->host), "");

    char default_port[8];
    snprintf(default_port, sizeof(default_port), "%u", (unsigned)BB_UDP_CLIENT_PORT);
    char port_str[8];
    bb_nv_get_str(BB_UDP_NVS_NS, "port", port_str, sizeof(port_str), default_port);
    out->port = (uint16_t)atoi(port_str);

    char bcast_str[4];
    bb_nv_get_str(BB_UDP_NVS_NS, "broadcast", bcast_str, sizeof(bcast_str),
                  BB_UDP_CLIENT_BROADCAST ? "1" : "0");
    out->broadcast = (bcast_str[0] == '1');
}

void bb_udp_client_priv_save_to_nvs(const bb_udp_client_cfg_t *cfg)
{
    bb_nv_set_str(BB_UDP_NVS_NS, "host", cfg->host);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)cfg->port);
    bb_nv_set_str(BB_UDP_NVS_NS, "port", port_str);
    bb_nv_set_str(BB_UDP_NVS_NS, "broadcast", cfg->broadcast ? "1" : "0");
}
