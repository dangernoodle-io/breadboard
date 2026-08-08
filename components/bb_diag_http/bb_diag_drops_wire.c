// bb_diag_drops_wire — wire descriptor + fill for GET /api/diag/drops. See
// bb_diag_drops_wire_priv.h for the six-counter object shape this route
// produces (B1-1444 PR3).

#include "bb_diag_drops_wire_priv.h"

#include <stddef.h>
#include <string.h>

static const bb_serialize_field_t s_drops_wire_fields[6] = {
    { .key = "writer_dropped", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_drops_wire_t, writer_dropped) },
    { .key = "udp_dropped", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_drops_wire_t, udp_dropped) },
    { .key = "event_dropped", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_drops_wire_t, event_dropped) },
    { .key = "flush_timeouts", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_drops_wire_t, flush_timeouts) },
    { .key = "render_fail", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_drops_wire_t, render_fail) },
    { .key = "stale_discard", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_drops_wire_t, stale_discard) },
};

const bb_serialize_desc_t bb_diag_drops_wire_desc = {
    .type_name = "bb_diag_drops_wire_t",
    .fields    = s_drops_wire_fields,
    .n_fields  = sizeof(s_drops_wire_fields) / sizeof(s_drops_wire_fields[0]),
    .snap_size = sizeof(bb_diag_drops_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-3a meta-derivation feeder) —
// co-located JSON Schema companion to bb_diag_drops_wire_desc above, gated
// behind BB_SERIALIZE_META_HOST (see bb_diag_drops_wire_priv.h's banner).
// "required" here mirrors the "required" array of
// platform/espidf/bb_diag_http/bb_diag_http_routes.c's hand-authored
// s_drops_get_responses[] 200 literal (all six fields). See
// test_bb_diag_drops_wire_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_drops_wire_meta_rows[] = {
    { .key = "writer_dropped", .required = true },
    { .key = "udp_dropped", .required = true },
    { .key = "event_dropped", .required = true },
    { .key = "flush_timeouts", .required = true },
    { .key = "render_fail", .required = true },
    { .key = "stale_discard", .required = true },
};

const bb_serialize_desc_meta_t bb_diag_drops_wire_meta = {
    .type_name = "bb_diag_drops_wire_t",
    .rows      = s_drops_wire_meta_rows,
    .n_rows    = sizeof(s_drops_wire_meta_rows) / sizeof(s_drops_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// GET /api/diag/drops response schema. Config OFF (default) keeps
// BB_DIAG_DROPS_SCHEMA_LITERAL byte-identical -- config ON composes it at
// init from bb_diag_drops_wire_desc/bb_diag_drops_wire_meta via
// bb_serialize_meta_ensure_composed() -- see
// bb_diag_drops_wire_ensure_schema_patched() below. Accepted config-ON delta
// (mirrors bb_diag_heap_check_wire.c's own note): the composed body gains a
// trailing "additionalProperties":false (the engine always closes the
// outermost rendered object); config-OFF ships the untouched literal.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
// Sized with headroom over the expected composed body -- any future desync
// trips bb_serialize_meta_openapi_schema()'s own bounded-buffer
// BB_ERR_NO_SPACE contract instead of silently truncating.
static char s_diag_drops_schema_buf[384];
#else
static const char k_diag_drops_schema[] = BB_DIAG_DROPS_SCHEMA_LITERAL;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_diag_drops_wire_ensure_schema_patched(void)
{
    return bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                              &bb_diag_drops_wire_desc, &bb_diag_drops_wire_meta,
                                              s_diag_drops_schema_buf, sizeof(s_diag_drops_schema_buf));
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_diag_drops_wire_get_schema(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return s_diag_drops_schema_buf;
#else
    return k_diag_drops_schema;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
}

void bb_diag_drops_wire_fill(bb_diag_drops_wire_t *dst,
                              uint32_t writer_dropped, uint32_t udp_dropped,
                              uint32_t event_dropped, uint32_t flush_timeouts,
                              size_t render_fail, uint32_t stale_discard)
{
    memset(dst, 0, sizeof(*dst));
    dst->writer_dropped = (int64_t)writer_dropped;
    dst->udp_dropped     = (int64_t)udp_dropped;
    dst->event_dropped   = (int64_t)event_dropped;
    dst->flush_timeouts  = (int64_t)flush_timeouts;
    dst->render_fail     = (int64_t)render_fail;
    dst->stale_discard   = (int64_t)stale_discard;
}
