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

// ---------------------------------------------------------------------------
// SSE topic schema for "log" (B1-1059 SSE batch PR-3). Config OFF (default)
// keeps the pre-existing hand-authored literal, byte-identical -- RELOCATED
// here verbatim from platform/espidf/bb_log_event/bb_log_event.c (a pure
// relocation: same const data, served identically, no new runtime-compose
// symbols in this build variant). Config ON composes it at init from
// bb_log_event_line_wire_desc/bb_log_event_line_wire_meta via bb_
// serialize_meta_ensure_topic_schema() -- see bb_log_event_line_ensure_
// schema_patched() below. The composed body picks up a top-level
// "additionalProperties":false the hand literal never had (the meta engine
// always closes every rendered object) -- a genuine config-ON tightening,
// consistent with every other B1-1059 compose-at-init site; config-OFF
// ships the untouched literal.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
// Sized with headroom over the golden-proven composed body (test_bb_log_
// event_line_wire_meta_golden.c's expected body is well under 300 bytes;
// the "LogEvent"/"log" title+topic prefix adds another ~40 bytes) -- any
// future desync trips bb_serialize_meta_openapi_schema()'s own bounded-
// buffer BB_ERR_NO_SPACE contract instead of silently truncating.
static char s_log_event_schema_buf[512];
#else
static const char k_log_event_schema[] =
    "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
    "\"properties\":{"
    "\"ts\":{\"type\":\"integer\"},"
    "\"level\":{\"type\":\"string\",\"enum\":[\"I\",\"W\",\"E\",\"D\",\"V\",\"?\"]},"
    "\"tag\":{\"type\":\"string\"},"
    "\"msg\":{\"type\":\"string\"}},"
    "\"required\":[\"ts\",\"level\",\"tag\",\"msg\"]}";
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_log_event_line_ensure_schema_patched(void)
{
    return bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                  &bb_log_event_line_wire_desc, &bb_log_event_line_wire_meta,
                                                  "LogEvent", "log",
                                                  s_log_event_schema_buf,
                                                  sizeof(s_log_event_schema_buf));
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_log_event_line_get_schema(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return s_log_event_schema_buf;
#else
    return k_log_event_schema;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
}
