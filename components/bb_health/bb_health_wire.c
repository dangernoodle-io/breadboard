// bb_health_wire — v2 wire descriptor (SSOT) for the /api/health payload.
// See bb_health_wire_priv.h for the byte-fidelity contract. Compiles on
// both host and ESP-IDF; no platform-specific code.

#include "bb_health_wire_priv.h"

#include <stddef.h>

static const bb_serialize_field_t s_health_net_wire_fields[] = {
    { .key = "ssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_health_net_wire_t, ssid), .max_len = 33 },
    { .key = "bssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_health_net_wire_t, bssid), .max_len = 18 },
    { .key = "ip", .type = BB_TYPE_STR,
      .offset = offsetof(bb_health_net_wire_t, ip), .max_len = 16 },
    { .key = "connected", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_health_net_wire_t, connected) },
    { .key = "mdns", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_health_net_wire_t, mdns) },
};

static const bb_serialize_field_t s_health_wire_fields[] = {
    { .key = "ok", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_health_wire_t, ok) },
    { .key = "network", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_health_wire_t, network),
      .children = s_health_net_wire_fields, .n_children = 5 },
};

const bb_serialize_desc_t bb_health_wire_desc = {
    .type_name = "health",
    .fields    = s_health_wire_fields,
    .n_fields  = 2,
    .snap_size = sizeof(bb_health_wire_t),
};
