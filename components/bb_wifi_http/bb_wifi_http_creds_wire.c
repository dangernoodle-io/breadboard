// bb_wifi_http_creds_wire — wire descriptor (SSOT) for the PATCH /api/wifi
// credentials-apply request (B1-1178: extraction of
// platform/espidf/bb_wifi_http/bb_wifi_http_routes.c's file-scope
// s_wifi_creds_desc into a portable TU, mirroring bb_wifi_http_wire.c's
// GET-side precedent, B1-1059 PR-2a). Compiles on both host and ESP-IDF; no
// platform-specific code. See bb_wifi_http_creds_wire_priv.h for the
// buffer-sizing rationale.

#include "bb_wifi_http_creds_wire_priv.h"

#include <stddef.h>

static const bb_serialize_field_t s_wifi_http_creds_wire_fields[] = {
    { .key = "ssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_creds_wire_t, ssid),
      .max_len = sizeof(((bb_wifi_http_creds_wire_t *)0)->ssid) },
    { .key = "password", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_creds_wire_t, pass),
      .max_len = sizeof(((bb_wifi_http_creds_wire_t *)0)->pass) },
};

const bb_serialize_desc_t bb_wifi_http_creds_wire_desc = {
    .type_name = "bb_wifi_http_creds_wire_t",
    .fields    = s_wifi_http_creds_wire_fields,
    .n_fields  = sizeof(s_wifi_http_creds_wire_fields) / sizeof(s_wifi_http_creds_wire_fields[0]),
    .snap_size = sizeof(bb_wifi_http_creds_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1178) -- co-located JSON Schema companion to
// bb_wifi_http_creds_wire_desc above, gated behind BB_SERIALIZE_META_HOST
// (see bb_wifi_http_creds_wire_priv.h's banner for the full mechanism doc).
// "required" here mirrors the "required" array of
// platform/espidf/bb_wifi_http/bb_wifi_http_routes.c's hand-authored
// s_wifi_patch_route.request_schema literal (["ssid"]) -- see
// test_bb_wifi_http_creds_wire_meta_golden.c for the fidelity proof.
// "maxLength" (31/63, B1-1186) mirrors the hand literal's true validation
// bound (BB_WIFI_PENDING_SSID_MAX/PASS_MAX) -- deliberately SMALLER than
// the base descriptor's oversized wire max_len (64/96, see
// bb_wifi_http_creds_wire_priv.h's buffer-sizing rationale), so the schema
// documents the real accept/reject boundary, not the padded wire buffer.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_wifi_http_creds_wire_meta_rows[] = {
    { .key = "ssid", .required = true, .max_len = BB_WIFI_PENDING_SSID_MAX },
    { .key = "password", .max_len = BB_WIFI_PENDING_PASS_MAX },
};

const bb_serialize_desc_meta_t bb_wifi_http_creds_wire_meta = {
    .type_name = "bb_wifi_http_creds_wire_t",
    .rows      = s_wifi_http_creds_wire_meta_rows,
    .n_rows    = sizeof(s_wifi_http_creds_wire_meta_rows) / sizeof(s_wifi_http_creds_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */
