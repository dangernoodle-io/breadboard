// bb_health_stack_wire — the format-agnostic "health.stack" descriptor SSOT.
// See bb_health_stack_wire.h for the wire-struct contract. Compiles on both
// host and ESP-IDF; no platform-specific code (the ESP-IDF-only gather lives
// in platform/espidf/bb_health/bb_health_stack.c, next to the s_last_stack
// stash it reads).

#include "bb_health_stack_wire.h"

#include <stddef.h>

static const bb_serialize_field_t s_health_stack_wire_fields[] = {
    { .key = "task", .type = BB_TYPE_STR,
      .offset = offsetof(bb_health_stack_wire_t, task),
      .max_len = sizeof(((bb_health_stack_wire_t *)0)->task) },
    { .key = "free_bytes", .type = BB_TYPE_I64,
      .offset = offsetof(bb_health_stack_wire_t, free_bytes) },
    { .key = "low", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_health_stack_wire_t, low) },
};

const bb_serialize_desc_t bb_health_stack_wire_desc = {
    .type_name = "health_stack",
    .fields    = s_health_stack_wire_fields,
    .n_fields  = sizeof(s_health_stack_wire_fields) / sizeof(s_health_stack_wire_fields[0]),
    .snap_size = sizeof(bb_health_stack_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-2) -- co-located JSON Schema
// companion to bb_health_stack_wire_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_health_stack_wire.h's banner). "required"
// mirrors the "required" array of platform/espidf/bb_health/
// bb_health_stack.c's hand-authored k_health_stack_schema literal
// (["task","free_bytes","low"]). See
// test_bb_health_stack_wire_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_health_stack_wire_meta_rows[] = {
    { .key = "task",       .required = true },
    { .key = "free_bytes", .required = true },
    { .key = "low",        .required = true },
};

const bb_serialize_desc_meta_t bb_health_stack_wire_meta = {
    .type_name = "health_stack",
    .rows      = s_health_stack_wire_meta_rows,
    .n_rows    = sizeof(s_health_stack_wire_meta_rows) / sizeof(s_health_stack_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */
