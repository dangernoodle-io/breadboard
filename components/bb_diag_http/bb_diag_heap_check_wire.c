// bb_diag_heap_check_wire — wire descriptor + fill for GET
// /api/diag/heap-check. See bb_diag_heap_check_wire_priv.h for the
// {"integrity_ok":<bool>} shape this migration produces.

#include "bb_diag_heap_check_wire_priv.h"

#include <stddef.h>
#include <string.h>

static const bb_serialize_field_t s_heap_check_wire_fields[1] = {
    { .key = "integrity_ok", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_heap_check_wire_t, integrity_ok) },
};

const bb_serialize_desc_t bb_diag_heap_check_wire_desc = {
    .type_name = "bb_diag_heap_check_wire_t",
    .fields    = s_heap_check_wire_fields,
    .n_fields  = sizeof(s_heap_check_wire_fields) / sizeof(s_heap_check_wire_fields[0]),
    .snap_size = sizeof(bb_diag_heap_check_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-3a meta-derivation feeder) —
// co-located JSON Schema companion to bb_diag_heap_check_wire_desc above,
// gated behind BB_SERIALIZE_META_HOST (see bb_diag_heap_check_wire_priv.h's
// banner). "required" here mirrors the "required" array of
// platform/espidf/bb_diag_http/bb_diag_http_routes.c's hand-authored
// s_heap_check_get_responses[] 200 literal (["integrity_ok"]). See
// test_bb_diag_heap_check_wire_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_heap_check_wire_meta_rows[] = {
    { .key = "integrity_ok", .required = true },
};

const bb_serialize_desc_meta_t bb_diag_heap_check_wire_meta = {
    .type_name = "bb_diag_heap_check_wire_t",
    .rows      = s_heap_check_wire_meta_rows,
    .n_rows    = sizeof(s_heap_check_wire_meta_rows) / sizeof(s_heap_check_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// GET /api/diag/heap-check response schema (B1-1059 emit batch C, site C2).
// Config OFF (default) keeps BB_DIAG_HEAP_CHECK_SCHEMA_LITERAL
// byte-identical -- config ON composes it at init from
// bb_diag_heap_check_wire_desc/bb_diag_heap_check_wire_meta via
// bb_serialize_meta_ensure_composed() -- see
// bb_diag_heap_check_wire_ensure_schema_patched() below. Accepted config-ON
// delta (see test_bb_diag_heap_check_wire_meta_golden.c): the composed body
// gains a trailing "additionalProperties":false (the engine always closes
// the outermost rendered object); config-OFF ships the untouched literal.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
// Sized with headroom over the golden-proven composed body
// (test_bb_diag_heap_check_wire_meta_golden.c's k_expected_meta_schema is
// 123 bytes incl. NUL) -- any future desync trips bb_serialize_meta_openapi_
// schema()'s own bounded-buffer BB_ERR_NO_SPACE contract instead of
// silently truncating.
static char s_diag_heap_check_schema_buf[192];
#else
static const char k_diag_heap_check_schema[] = BB_DIAG_HEAP_CHECK_SCHEMA_LITERAL;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_diag_heap_check_wire_ensure_schema_patched(void)
{
    return bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                              &bb_diag_heap_check_wire_desc, &bb_diag_heap_check_wire_meta,
                                              s_diag_heap_check_schema_buf, sizeof(s_diag_heap_check_schema_buf));
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_diag_heap_check_wire_get_schema(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return s_diag_heap_check_schema_buf;
#else
    return k_diag_heap_check_schema;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
}

void bb_diag_heap_check_wire_fill(bb_diag_heap_check_wire_t *dst, bool integrity_ok)
{
    memset(dst, 0, sizeof(*dst));
    dst->integrity_ok = integrity_ok;
}
