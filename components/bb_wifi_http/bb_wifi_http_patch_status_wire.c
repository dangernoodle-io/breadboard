// bb_wifi_http_patch_status_wire — wire descriptor (SSOT) for the PATCH
// /api/wifi 202 success response body (B1-1286, firmware-review follow-up:
// extraction of platform/espidf/bb_wifi_http/bb_wifi_http_routes.c's
// file-scope s_wifi_patch_status_wire_desc into a portable TU, mirroring
// bb_wifi_http_creds_wire.c's request-side precedent, B1-1178). Compiles on
// both host and ESP-IDF; no platform-specific code.

#include "bb_wifi_http_patch_status_wire_priv.h"

#include <stddef.h>

static const bb_serialize_field_t s_wifi_http_patch_status_wire_fields[] = {
    { .key = "status", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_patch_status_wire_t, status),
      .max_len = sizeof(((bb_wifi_http_patch_status_wire_t *)0)->status) },
};

const bb_serialize_desc_t bb_wifi_http_patch_status_wire_desc = {
    .type_name = "bb_wifi_http_patch_status_wire_t",
    .fields    = s_wifi_http_patch_status_wire_fields,
    .n_fields  = sizeof(s_wifi_http_patch_status_wire_fields) / sizeof(s_wifi_http_patch_status_wire_fields[0]),
    .snap_size = sizeof(bb_wifi_http_patch_status_wire_t),
};
