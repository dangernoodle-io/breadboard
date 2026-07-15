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
// on-flash NVS entry type and strand provisioned boards. Only the key/type
// triples below are byte-compatible with what this component previously
// wrote — namespace is now caller-supplied (B1-951 — the component declares
// WHAT it stores, the composition decides WHERE; see bb_tls_creds's
// resolve_one for the reference pattern).
#include "bb_udp_client_priv.h"
#include "bb_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Decimal-ASCII port default derived from BB_UDP_CLIENT_PORT at compile time
// (stringified), matching the prior snprintf("%u", BB_UDP_CLIENT_PORT)
// fallback exactly.
#define BB_UDP_STR2(x) #x
#define BB_UDP_STR(x)  BB_UDP_STR2(x)

void bb_udp_client_priv_load_from_nvs(const char *ns, bb_udp_client_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    // ns is caller-supplied at runtime, so these field descriptors are built
    // per-call rather than declared static const — bb_config_field_t carries
    // no state of its own, so a stack-local instance is safe to pass by
    // pointer for the duration of this single call (mirrors bb_tls_creds's
    // resolve_one).
    const bb_config_field_t host_field = {
        .id          = "udp.host",
        .type        = BB_CONFIG_STR,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "host" },
        .max_len     = BB_UDP_CLIENT_HOST_MAX,
        .def         = { .str = "" },
        .has_default = true,
    };
    const bb_config_field_t port_field = {
        .id          = "udp.port",
        .type        = BB_CONFIG_STR,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "port" },
        .max_len     = 8,
        .def         = { .str = BB_UDP_STR(BB_UDP_CLIENT_PORT) },
        .has_default = true,
    };
    const bb_config_field_t broadcast_field = {
        .id          = "udp.broadcast",
        .type        = BB_CONFIG_STR,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "broadcast" },
        .max_len     = 4,
        .def         = { .str = BB_UDP_CLIENT_BROADCAST ? "1" : "0" },
        .has_default = true,
    };

    size_t out_len = 0;
    bb_config_get_str(&host_field, out->host, sizeof(out->host), &out_len);

    char port_str[8] = {0};
    bb_config_get_str(&port_field, port_str, sizeof(port_str), &out_len);
    out->port = (uint16_t)atoi(port_str);

    char bcast_str[4] = {0};
    bb_config_get_str(&broadcast_field, bcast_str, sizeof(bcast_str), &out_len);
    out->broadcast = (bcast_str[0] == '1');
}

void bb_udp_client_priv_save_to_nvs(const char *ns, const bb_udp_client_cfg_t *cfg)
{
    const bb_config_field_t host_field = {
        .id          = "udp.host",
        .type        = BB_CONFIG_STR,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "host" },
        .max_len     = BB_UDP_CLIENT_HOST_MAX,
        .def         = { .str = "" },
        .has_default = true,
    };
    const bb_config_field_t port_field = {
        .id          = "udp.port",
        .type        = BB_CONFIG_STR,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "port" },
        .max_len     = 8,
        .def         = { .str = BB_UDP_STR(BB_UDP_CLIENT_PORT) },
        .has_default = true,
    };
    const bb_config_field_t broadcast_field = {
        .id          = "udp.broadcast",
        .type        = BB_CONFIG_STR,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "broadcast" },
        .max_len     = 4,
        .def         = { .str = BB_UDP_CLIENT_BROADCAST ? "1" : "0" },
        .has_default = true,
    };

    bb_config_set_str(&host_field, cfg->host);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)cfg->port);
    bb_config_set_str(&port_field, port_str);
    bb_config_set_str(&broadcast_field, cfg->broadcast ? "1" : "0");
}
