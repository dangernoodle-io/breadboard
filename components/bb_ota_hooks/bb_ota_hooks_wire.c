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

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-2) -- co-located JSON Schema
// companion to bb_ota_hooks_wire_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_ota_hooks_wire.h's banner). "required"
// mirrors the "required" array of platform/espidf/bb_ota_hooks/
// bb_ota_hooks.c's hand-authored k_ota_progress_schema literal
// (["via","state","pct"]); "state"'s enum_vals mirrors that same literal's
// "enum":["start","progress","success","fail","unknown"]. See
// test_bb_ota_hooks_wire_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const char *const s_ota_hooks_state_enum_vals[] = {
    "start", "progress", "success", "fail", "unknown", NULL,
};

static const bb_serialize_field_meta_t s_ota_hooks_wire_meta_rows[] = {
    { .key = "via",   .required = true },
    { .key = "state", .required = true, .enum_vals = s_ota_hooks_state_enum_vals },
    { .key = "pct",   .required = true },
};

const bb_serialize_desc_meta_t bb_ota_hooks_wire_meta = {
    .type_name = "ota_progress",
    .rows      = s_ota_hooks_wire_meta_rows,
    .n_rows    = sizeof(s_ota_hooks_wire_meta_rows) / sizeof(s_ota_hooks_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// SSE topic schema for "ota.progress" (B1-1059 SSE batch PR-3). Config OFF
// (default) keeps the pre-existing hand-authored literal, byte-identical --
// RELOCATED here verbatim from platform/espidf/bb_ota_hooks/bb_ota_hooks.c
// (a pure relocation: same const data, served identically, no new runtime-
// compose symbols in this build variant). Config ON composes it at init
// from bb_ota_hooks_wire_desc/bb_ota_hooks_wire_meta via bb_serialize_meta_
// ensure_topic_schema() -- see bb_ota_hooks_ensure_schema_patched() below.
// The composed body picks up a top-level "additionalProperties":false the
// hand literal never had (the meta engine always closes every rendered
// object) -- a genuine config-ON tightening, consistent with every other
// B1-1059 compose-at-init site; config-OFF ships the untouched literal.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
// Sized with headroom over the golden-proven composed body (test_bb_ota_
// hooks_wire_meta_golden.c's expected body is well under 300 bytes; the
// "OtaProgress"/"ota.progress" title+topic prefix adds another ~50 bytes)
// -- any future desync trips bb_serialize_meta_openapi_schema()'s own
// bounded-buffer BB_ERR_NO_SPACE contract instead of silently truncating.
static char s_ota_hooks_schema_buf[512];
#else
static const char k_ota_progress_schema[] =
    "{\"title\":\"OtaProgress\",\"x-sse-topic\":\"ota.progress\",\"type\":\"object\","
    "\"properties\":{"
    "\"via\":{\"type\":\"string\"},"
    "\"state\":{\"type\":\"string\","
    "\"enum\":[\"start\",\"progress\",\"success\",\"fail\",\"unknown\"]},"
    "\"pct\":{\"type\":\"integer\"}},"
    "\"required\":[\"via\",\"state\",\"pct\"]}";
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_ota_hooks_ensure_schema_patched(void)
{
    return bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                  &bb_ota_hooks_wire_desc, &bb_ota_hooks_wire_meta,
                                                  "OtaProgress", "ota.progress",
                                                  s_ota_hooks_schema_buf,
                                                  sizeof(s_ota_hooks_schema_buf));
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_ota_hooks_get_schema(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return s_ota_hooks_schema_buf;
#else
    return k_ota_progress_schema;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
}
