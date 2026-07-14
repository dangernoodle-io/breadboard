// bb_udp_client — common logic shared by the host and ESP-IDF backends:
// NVS-backed dest-config load/save. The platform-specific files
// (platform/{host,espidf}/bb_udp_client/*.c) implement init state and the
// actual datagram transport (real sendto() vs. host capture buffer).
//
// host/port/broadcast round-trip through bb_config (typed layer over
// bb_storage) rather than bb_nv's generic KV forwarder (B1-756, bb_nv
// dissolution epic B1-708) — bb_config's STR encoding resolves to the SAME
// nvs_set_str call bb_nv_set_str made (both are thin forwarders to
// bb_storage_nvs, see bb_storage_nvs.h). port/broadcast keep their existing
// STR-typed decimal-ASCII encoding ("9109" / "0"|"1") byte-for-byte — do NOT
// switch them to bb_config's U16/BOOL typed encoding, which would change the
// on-flash NVS entry type and strand provisioned boards.
#include "bb_udp_client_priv.h"
#include "bb_config.h"
#include "bb_nv_namespaces.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Namespace/keys byte-for-byte matched to bb_nv's prior BB_UDP_NVS_NS
// ("bb_udp")/"host"/"port"/"broadcast" — do not change without a migration
// plan, this strands provisioned-board UDP transport config otherwise.
static const bb_config_field_t s_udp_host_field = {
    .id          = "udp.host",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_UDP_NVS_NS, .key = "host" },
    .max_len     = BB_UDP_CLIENT_HOST_MAX,
    .def         = { .str = "" },
    .has_default = true,
};

// Decimal-ASCII port default derived from BB_UDP_CLIENT_PORT at compile time
// (stringified), matching the prior snprintf("%u", BB_UDP_CLIENT_PORT)
// fallback exactly.
#define BB_UDP_STR2(x) #x
#define BB_UDP_STR(x)  BB_UDP_STR2(x)

static const bb_config_field_t s_udp_port_field = {
    .id          = "udp.port",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_UDP_NVS_NS, .key = "port" },
    .max_len     = 8,
    .def         = { .str = BB_UDP_STR(BB_UDP_CLIENT_PORT) },
    .has_default = true,
};

static const bb_config_field_t s_udp_broadcast_field = {
    .id          = "udp.broadcast",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_UDP_NVS_NS, .key = "broadcast" },
    .max_len     = 4,
    .def         = { .str = BB_UDP_CLIENT_BROADCAST ? "1" : "0" },
    .has_default = true,
};

void bb_udp_client_priv_load_from_nvs(bb_udp_client_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    size_t out_len = 0;
    bb_config_get_str(&s_udp_host_field, out->host, sizeof(out->host), &out_len);

    char port_str[8] = {0};
    bb_config_get_str(&s_udp_port_field, port_str, sizeof(port_str), &out_len);
    out->port = (uint16_t)atoi(port_str);

    char bcast_str[4] = {0};
    bb_config_get_str(&s_udp_broadcast_field, bcast_str, sizeof(bcast_str), &out_len);
    out->broadcast = (bcast_str[0] == '1');
}

void bb_udp_client_priv_save_to_nvs(const bb_udp_client_cfg_t *cfg)
{
    bb_config_set_str(&s_udp_host_field, cfg->host);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)cfg->port);
    bb_config_set_str(&s_udp_port_field, port_str);
    bb_config_set_str(&s_udp_broadcast_field, cfg->broadcast ? "1" : "0");
}
