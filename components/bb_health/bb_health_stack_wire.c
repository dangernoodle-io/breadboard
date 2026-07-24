// bb_health_stack_wire — the format-agnostic "health.stack" descriptor SSOT.
// See bb_health_stack_wire.h for the wire-struct contract. Compiles on both
// host and ESP-IDF; no platform-specific code (the ESP-IDF-only gather lives
// in platform/espidf/bb_health/bb_health_stack.c, next to the s_last_stack
// stash it reads).

#include "bb_health_stack_wire.h"

#include "bb_health_stack.h"

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

// ---------------------------------------------------------------------------
// SSE topic schema for "health.stack" (B1-1059 SSE batch PR-3). Config OFF
// (default) keeps the pre-existing hand-authored literal, byte-identical --
// RELOCATED here verbatim from platform/espidf/bb_health/bb_health_stack.c
// (a pure relocation: same const data, served identically, no new runtime-
// compose symbols in this build variant). Config ON composes it at init
// from bb_health_stack_wire_desc/bb_health_stack_wire_meta via bb_
// serialize_meta_ensure_topic_schema() -- see bb_health_stack_ensure_
// schema_patched() below. The composed body picks up a top-level
// "additionalProperties":false the hand literal never had (the meta engine
// always closes every rendered object) -- a genuine config-ON tightening,
// consistent with every other B1-1059 compose-at-init site; config-OFF
// ships the untouched literal.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
// Sized with headroom over the golden-proven composed body (test_bb_health_
// stack_wire_meta_golden.c's expected body is well under 200 bytes; the
// "HealthStack"/"health.stack" title+topic prefix adds another ~50 bytes)
// -- any future desync trips bb_serialize_meta_openapi_schema()'s own
// bounded-buffer BB_ERR_NO_SPACE contract instead of silently truncating.
static char s_health_stack_schema_buf[512];
#else
static const char k_health_stack_schema[] =
    "{\"title\":\"HealthStack\",\"x-sse-topic\":\"health.stack\",\"type\":\"object\","
    "\"properties\":{"
    "\"task\":{\"type\":\"string\"},"
    "\"free_bytes\":{\"type\":\"integer\"},"
    "\"low\":{\"type\":\"boolean\"}},"
    "\"required\":[\"task\",\"free_bytes\",\"low\"]}";
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_health_stack_ensure_schema_patched(void)
{
    return bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                  &bb_health_stack_wire_desc, &bb_health_stack_wire_meta,
                                                  "HealthStack", BB_HEALTH_STACK_TOPIC,
                                                  s_health_stack_schema_buf,
                                                  sizeof(s_health_stack_schema_buf));
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_health_stack_get_schema(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return s_health_stack_schema_buf;
#else
    return k_health_stack_schema;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
}
