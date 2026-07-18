// bb_ota_hooks_wire — the format-agnostic "ota.progress" descriptor SSOT.
// See bb_ota_hooks_wire.h for the wire-struct contract. Compiles on both
// host and ESP-IDF; no platform-specific code. bb_ota_hooks_gather() (the
// stash reader) lives in bb_ota_hooks.c, next to the s_last_phase/s_last_pct/
// s_last_via statics it reads.

#include "bb_ota_hooks_wire.h"

#include <stddef.h>

static const bb_serialize_field_t s_ota_hooks_wire_fields[] = {
    { .key = "via", .type = BB_TYPE_STR,
      .offset = offsetof(bb_ota_hooks_wire_t, via), .max_len = sizeof(((bb_ota_hooks_wire_t *)0)->via) },
    { .key = "state", .type = BB_TYPE_STR,
      .offset = offsetof(bb_ota_hooks_wire_t, state), .max_len = sizeof(((bb_ota_hooks_wire_t *)0)->state) },
    { .key = "pct", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ota_hooks_wire_t, pct) },
};

const bb_serialize_desc_t bb_ota_hooks_wire_desc = {
    .type_name = "ota_progress",
    .fields    = s_ota_hooks_wire_fields,
    .n_fields  = sizeof(s_ota_hooks_wire_fields) / sizeof(s_ota_hooks_wire_fields[0]),
    .snap_size = sizeof(bb_ota_hooks_wire_t),
};
