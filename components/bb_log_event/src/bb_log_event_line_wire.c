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

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-2) -- co-located JSON Schema
// companion to bb_log_event_line_wire_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_log_event_line_wire_priv.h's banner).
// "required" mirrors the "required" array of platform/espidf/bb_log_event/
// bb_log_event.c's hand-authored k_log_event_schema literal
// (["ts","level","tag","msg"]); "level"'s enum_vals mirrors that same
// literal's "enum":["I","W","E","D","V","?"]. See
// test_bb_log_event_line_wire_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const char *const s_log_event_level_enum_vals[] = {
    "I", "W", "E", "D", "V", "?", NULL,
};

static const bb_serialize_field_meta_t s_log_event_line_wire_meta_rows[] = {
    { .key = "ts",    .required = true },
    { .key = "level", .required = true, .enum_vals = s_log_event_level_enum_vals },
    { .key = "tag",   .required = true },
    { .key = "msg",   .required = true },
};

const bb_serialize_desc_meta_t bb_log_event_line_wire_meta = {
    .type_name = "bb_log_event_line_wire_t",
    .rows      = s_log_event_line_wire_meta_rows,
    .n_rows    = sizeof(s_log_event_line_wire_meta_rows) / sizeof(s_log_event_line_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */
