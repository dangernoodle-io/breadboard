// bb_storage_http_delete_wire — wire descriptor + fill for the DELETE
// /api/diag/storage response. See bb_storage_http_delete_wire_priv.h for
// the {"deleted":[...],"key":...} shape this migration produces.

#include "bb_storage_http_delete_wire_priv.h"

#include <stddef.h>
#include <string.h>

// present-predicate for the "key" field -- omits it entirely unless the
// request named a single key (has_key), same contract as the pre-migration
// bb_json_obj_set_string() call it replaces (only invoked inside
// `if (has_key)`).
static bool delete_wire_key_present(const void *snap)
{
    const bb_storage_http_delete_wire_t *w = (const bb_storage_http_delete_wire_t *)snap;
    return w->has_key;
}

static const bb_serialize_field_t s_delete_wire_fields[2] = {
    { .key = "deleted", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_storage_http_delete_wire_t, deleted),
      .elem_type = BB_TYPE_STR,
      .max_len = sizeof(((bb_storage_http_delete_wire_t *)0)->deleted_names[0]),
      .max_items = BB_STORAGE_HTTP_DELETE_NS_MAX },
    { .key = "key", .type = BB_TYPE_STR,
      .offset = offsetof(bb_storage_http_delete_wire_t, key),
      .max_len = sizeof(((bb_storage_http_delete_wire_t *)0)->key),
      .present = delete_wire_key_present },
};

const bb_serialize_desc_t bb_storage_http_delete_wire_desc = {
    .type_name = "bb_storage_http_delete_wire_t",
    .fields    = s_delete_wire_fields,
    .n_fields  = sizeof(s_delete_wire_fields) / sizeof(s_delete_wire_fields[0]),
    .snap_size = sizeof(bb_storage_http_delete_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-1) -- co-located JSON Schema
// companion to bb_storage_http_delete_wire_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_storage_http_delete_wire_priv.h's banner).
// "required" here mirrors the "required" array of
// platform/espidf/bb_diag_http/bb_storage_http_routes.c's hand-authored
// s_storage_delete_responses[] 200 literal (["deleted"] -- "key" is
// conditionally present, never required). See
// test_bb_storage_http_delete_wire_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_delete_wire_meta_rows[] = {
    { .key = "deleted", .required = true },
    { .key = "key" },
};

const bb_serialize_desc_meta_t bb_storage_http_delete_wire_meta = {
    .type_name = "bb_storage_http_delete_wire_t",
    .rows      = s_delete_wire_meta_rows,
    .n_rows    = sizeof(s_delete_wire_meta_rows) / sizeof(s_delete_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage 200 response schema (B1-1059 emit batch C, site
// C5). Config OFF (default) keeps
// BB_STORAGE_HTTP_DELETE_RESPONSE_SCHEMA_LITERAL byte-identical -- config ON
// composes it at init from bb_storage_http_delete_wire_desc/
// bb_storage_http_delete_wire_meta via bb_serialize_meta_ensure_composed()
// -- see bb_storage_http_delete_wire_ensure_schema_patched() below.
// Accepted config-ON delta (see test_bb_storage_http_delete_wire_meta_
// golden.c): the composed body gains a trailing "additionalProperties":
// false (the engine always closes the outermost rendered object);
// config-OFF ships the untouched literal.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
// Sized with headroom over the golden-proven composed body
// (test_bb_storage_http_delete_wire_meta_golden.c's k_expected_meta_schema
// is well under 256 bytes) -- any future desync trips bb_serialize_meta_
// openapi_schema()'s own bounded-buffer BB_ERR_NO_SPACE contract instead of
// silently truncating.
static char s_storage_http_delete_response_schema_buf[256];
#else
static const char k_storage_http_delete_response_schema[] = BB_STORAGE_HTTP_DELETE_RESPONSE_SCHEMA_LITERAL;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_storage_http_delete_wire_ensure_schema_patched(void)
{
    return bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                              &bb_storage_http_delete_wire_desc, &bb_storage_http_delete_wire_meta,
                                              s_storage_http_delete_response_schema_buf,
                                              sizeof(s_storage_http_delete_response_schema_buf));
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_storage_http_delete_wire_get_schema(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return s_storage_http_delete_response_schema_buf;
#else
    return k_storage_http_delete_response_schema;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
}

void bb_storage_http_delete_wire_fill(bb_storage_http_delete_wire_t *dst,
                                       const char names[][BB_STORAGE_HTTP_DELETE_NS_LEN], size_t n,
                                       const char *key, bool has_key)
{
    memset(dst, 0, sizeof(*dst));

    for (size_t i = 0; i < n; i++) {
        strncpy(dst->deleted_names[i], names[i], sizeof(dst->deleted_names[i]) - 1);
        dst->deleted_names[i][sizeof(dst->deleted_names[i]) - 1] = '\0';
        dst->deleted_items[i] = dst->deleted_names[i];
    }
    dst->deleted.items = dst->deleted_items;
    dst->deleted.count = n;

    if (has_key && key) {
        strncpy(dst->key, key, sizeof(dst->key) - 1);
        dst->key[sizeof(dst->key) - 1] = '\0';
        dst->has_key = true;
    }
}
