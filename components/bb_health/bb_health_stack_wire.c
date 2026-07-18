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
