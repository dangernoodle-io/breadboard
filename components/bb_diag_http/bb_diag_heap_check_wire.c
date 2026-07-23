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
#if defined(BB_SERIALIZE_META_HOST)

static const bb_serialize_field_meta_t s_heap_check_wire_meta_rows[] = {
    { .key = "integrity_ok", .required = true },
};

const bb_serialize_desc_meta_t bb_diag_heap_check_wire_meta = {
    .type_name = "bb_diag_heap_check_wire_t",
    .rows      = s_heap_check_wire_meta_rows,
    .n_rows    = sizeof(s_heap_check_wire_meta_rows) / sizeof(s_heap_check_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_HOST */

void bb_diag_heap_check_wire_fill(bb_diag_heap_check_wire_t *dst, bool integrity_ok)
{
    memset(dst, 0, sizeof(*dst));
    dst->integrity_ok = integrity_ok;
}
