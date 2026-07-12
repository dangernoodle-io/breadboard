// bb_info_wire — v2 wire descriptor (SSOT) for the /api/info root payload.
// See bb_info_wire_priv.h for the byte-fidelity contract. Compiles on both
// host and ESP-IDF; no platform-specific code.

#include "../bb_info_wire_priv.h"

#include <stddef.h>

static const bb_serialize_field_t s_info_wire_fields[] = {
    { .key = "mac", .type = BB_TYPE_STR,
      .offset = offsetof(bb_info_wire_t, mac), .max_len = 18 },
    { .key = "ota_validated", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_info_wire_t, ota_validated) },
    { .key = "time_valid", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_info_wire_t, time_valid) },
    { .key = "boot_epoch_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_info_wire_t, boot_epoch_s) },
    { .key = "time_source", .type = BB_TYPE_STR,
      .offset = offsetof(bb_info_wire_t, time_source), .max_len = 8 },
    { .key = "hostname", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_info_wire_t, hostname) },
    { .key = "capabilities", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_info_wire_t, capabilities),
      .elem_type = BB_TYPE_STR, .max_len = 32 },
};

const bb_serialize_desc_t bb_info_wire_desc = {
    .type_name = "info",
    .fields    = s_info_wire_fields,
    .n_fields  = 7,
    .snap_size = sizeof(bb_info_wire_t),
};
