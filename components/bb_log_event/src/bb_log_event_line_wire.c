// bb_log_event_line_wire — the format-agnostic per-log-line descriptor SSOT.
// See bb_log_event_line_wire_priv.h for the wire-struct contract. Compiles
// on both host and ESP-IDF; no platform-specific code (the ESP-IDF-only
// forwarder that renders through this descriptor lives in bb_log_event.c).

#include "../bb_log_event_line_wire_priv.h"

#include <stddef.h>

const bb_serialize_field_t bb_log_event_line_wire_fields[4] = {
    { .key = "ts", .type = BB_TYPE_I64,
      .offset = offsetof(bb_log_event_line_wire_t, ts) },
    { .key = "level", .type = BB_TYPE_STR,
      .offset = offsetof(bb_log_event_line_wire_t, level),
      .max_len = sizeof(((bb_log_event_line_wire_t *)0)->level) },
    { .key = "tag", .type = BB_TYPE_STR,
      .offset = offsetof(bb_log_event_line_wire_t, tag),
      .max_len = sizeof(((bb_log_event_line_wire_t *)0)->tag) },
    { .key = "msg", .type = BB_TYPE_STR,
      .offset = offsetof(bb_log_event_line_wire_t, msg),
      .max_len = sizeof(((bb_log_event_line_wire_t *)0)->msg) },
};

const uint16_t bb_log_event_line_wire_n_fields =
    sizeof(bb_log_event_line_wire_fields) / sizeof(bb_log_event_line_wire_fields[0]);

const bb_serialize_desc_t bb_log_event_line_wire_desc = {
    .type_name = "bb_log_event_line_wire_t",
    .fields    = bb_log_event_line_wire_fields,
    .n_fields  = sizeof(bb_log_event_line_wire_fields) / sizeof(bb_log_event_line_wire_fields[0]),
    .snap_size = sizeof(bb_log_event_line_wire_t),
};
