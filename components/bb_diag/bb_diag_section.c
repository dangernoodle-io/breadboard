// Portable section registry + pure dispatch helpers for the bb_diag section
// registry (B1-diag-dissolution PR3). See bb_diag_section.h for the public
// seam contract and bb_diag_section_priv.h for the internals shared with
// the ESP-IDF dispatcher and host tests. Stores the name -> section table in
// a bb_registry instance (name-keyed), same shape as bb_data.c's own
// binding table -- no hand-rolled fixed array + linear string scan.
#include "bb_diag_section_priv.h"

#include "bb_log.h"
#include "bb_registry.h"

#include <string.h>

static const char *TAG = "bb_diag_section";

#define BB_DIAG_SECTION_URI_PREFIX     "/api/diag/"
#define BB_DIAG_SECTION_URI_PREFIX_LEN (sizeof(BB_DIAG_SECTION_URI_PREFIX) - 1)

// One name's stored section -- a BY-VALUE copy of the caller's
// bb_diag_section_t (the caller's own struct may be a stack temporary),
// plus an owned copy of the name string (a stable, static-storage name
// pointer for the bb_registry entry -- the registry only borrows `name`,
// it never copies it).
typedef struct {
    bool               in_use;
    char               name[BB_DIAG_SECTION_NAME_MAX];
    bb_diag_section_t  section;
    // Cached at registration time for an `iter` section (the validated
    // stream field's elem_size); 0 for a `fill` section (unused there).
    // See bb_diag_section_stream_elem_size()'s doc for why this lives here
    // rather than being re-scanned per request.
    size_t             stream_elem_size;
} bb_diag_section_slot_t;

static bb_diag_section_slot_t s_slots[BB_DIAG_SECTION_TABLE_CAP];

BB_REGISTRY_DEFINE_TAGGED(s_bb_diag_section_registry, BB_DIAG_SECTION_TABLE_CAP, "bb_diag_section");

// First slot with in_use == false. Guaranteed to find one whenever
// bb_registry_count(&s_bb_diag_section_registry) < BB_DIAG_SECTION_TABLE_CAP,
// since every successful bb_registry_register() call below is paired 1:1
// with claiming exactly one slot here (and neither table supports removal).
static bb_diag_section_slot_t *find_free_slot(void)
{
    for (size_t i = 0; i < BB_DIAG_SECTION_TABLE_CAP; i++) {
        if (!s_slots[i].in_use) return &s_slots[i];
    }
    return NULL;
}

// Rejects any BB_TYPE_ARR field with cardinality == BB_ARR_STREAM found
// BELOW the top level (depth >= 1) of `fields` -- the section iter/
// dispatcher model wires the carrier ONLY at a TOP-LEVEL field's own offset
// in `dst` (bb_diag_section_dispatch.c calls bb_serialize_arr_stream_from_buf()
// against the section's own snap struct, one field, one offset); a STREAM
// field nested inside an OBJ child, an ARR-of-OBJ element's row shape, or a
// STREAM field's OWN row shape has no such wiring point. Left unvalidated,
// the walker would still drive it via bb_serialize_walk.c's BB_TYPE_ARR/
// BB_ARR_STREAM branch at that nesting depth -- with an elem_size that
// bypassed validate_stream_field()'s top-level BB_SERIALIZE_MAX_ROW_BYTES
// check entirely, so a large enough nested row would overrun the walker's
// `row_buf[BB_SERIALIZE_MAX_ROW_BYTES]` stack buffer.
//
// Recurses into BB_TYPE_OBJ children and BB_TYPE_ARR-of-BB_TYPE_OBJ
// children (any field carrying a `children` table, including a STREAM
// field's own row shape) bounded by BB_SERIALIZE_MAX_DEPTH, mirroring the
// walker's own recursion guard -- fields at or past that depth are
// unreachable by the walker itself, so scanning further is pointless.
static bb_err_t reject_nested_stream_fields(const bb_serialize_field_t *fields, uint16_t n_fields,
                                             unsigned depth)
{
    if (depth >= BB_SERIALIZE_MAX_DEPTH) return BB_OK;

    for (uint16_t i = 0; i < n_fields; i++) {
        const bb_serialize_field_t *f = &fields[i];

        if (f->type == BB_TYPE_ARR && f->cardinality == BB_ARR_STREAM) {
            return BB_ERR_INVALID_ARG;
        }
        if (f->type == BB_TYPE_OBJ || (f->type == BB_TYPE_ARR && f->elem_type == BB_TYPE_OBJ)) {
            bb_err_t rc = reject_nested_stream_fields(f->children, f->n_children, depth + 1);
            if (rc != BB_OK) return rc;
        }
    }
    return BB_OK;
}

// Validates `section->snap_desc`'s top-level BB_TYPE_ARR fields against the
// fill/iter contract (see bb_diag_register_section()'s doc): a `fill`
// section must have zero BB_ARR_STREAM fields (at ANY depth); an `iter`
// section must have EXACTLY ONE, at the TOP level, with elem_type ==
// BB_TYPE_OBJ and elem_size fitting BB_SERIALIZE_MAX_ROW_BYTES -- and no
// OTHER BB_ARR_STREAM field anywhere below the top level, in any child
// table (see reject_nested_stream_fields()). On success, `*out_elem_size`
// is the top-level stream field's elem_size for an `iter` section, or 0 for
// a `fill` section.
static bb_err_t validate_stream_field(const bb_diag_section_t *section, size_t *out_elem_size)
{
    const bb_serialize_desc_t *desc = section->snap_desc;
    size_t stream_count = 0;
    size_t elem_size = 0;

    for (uint16_t i = 0; i < desc->n_fields; i++) {
        const bb_serialize_field_t *f = &desc->fields[i];

        if (f->type == BB_TYPE_ARR && f->cardinality == BB_ARR_STREAM) {
            if (!section->iter) {
                // fill section: a STREAM field has no one-shot fill-time
                // scatter story -- only iter sections may wire one.
                return BB_ERR_INVALID_ARG;
            }

            stream_count++;
            if (f->elem_type != BB_TYPE_OBJ) return BB_ERR_INVALID_ARG;
            if (f->elem_size == 0) return BB_ERR_INVALID_ARG;
            if (f->elem_size > BB_SERIALIZE_MAX_ROW_BYTES) {
                bb_log_e(TAG, "register('%s'): stream field '%s' elem_size=%u exceeds "
                         "BB_SERIALIZE_MAX_ROW_BYTES=%u",
                         section->name, f->key, (unsigned)f->elem_size,
                         (unsigned)BB_SERIALIZE_MAX_ROW_BYTES);
                return BB_ERR_NO_SPACE;
            }
            elem_size = f->elem_size;
        }

        // Any field carrying a children table -- an OBJ, an ARR-of-OBJ
        // (FIXED or, above, the STREAM field just validated), or an ARR
        // whose cardinality/elem_type combination this loop doesn't
        // otherwise special-case -- may itself nest a STREAM field one or
        // more levels down. Reject it there; only a TOP-LEVEL STREAM field
        // has a wiring point.
        if (f->type == BB_TYPE_OBJ || (f->type == BB_TYPE_ARR && f->elem_type == BB_TYPE_OBJ)) {
            bb_err_t rc = reject_nested_stream_fields(f->children, f->n_children, 1);
            if (rc != BB_OK) return rc;
        }
    }

    if (section->iter && stream_count != 1) return BB_ERR_INVALID_ARG;

    *out_elem_size = elem_size;
    return BB_OK;
}

bb_err_t bb_diag_register_section(const bb_diag_section_t *section)
{
    if (!section || !section->name || !section->snap_desc) return BB_ERR_INVALID_ARG;
    if ((section->fill == NULL) == (section->iter == NULL)) return BB_ERR_INVALID_ARG;  // neither or both set
    if (strlen(section->name) >= BB_DIAG_SECTION_NAME_MAX) return BB_ERR_INVALID_ARG;
    if (section->n_query_keys > BB_SERIALIZE_QUERY_MAX_PARAMS) return BB_ERR_INVALID_ARG;
    if (section->n_query_keys > 0 && !section->query_keys) return BB_ERR_INVALID_ARG;

    if (section->snap_desc->snap_size > BB_DIAG_SECTION_SCRATCH_BYTES) {
        bb_log_e(TAG, "register('%s'): snap_size=%u exceeds scratch=%u -- "
                 "raise CONFIG_BB_DIAG_SECTION_SCRATCH_BYTES or shrink the snapshot",
                 section->name, (unsigned)section->snap_desc->snap_size,
                 (unsigned)BB_DIAG_SECTION_SCRATCH_BYTES);
        return BB_ERR_NO_SPACE;
    }

    size_t stream_elem_size = 0;
    bb_err_t rc = validate_stream_field(section, &stream_elem_size);
    if (rc != BB_OK) return rc;

    // Reject-on-duplicate, first-wins -- checked explicitly (rather than
    // relying solely on bb_registry_register()'s own duplicate rejection)
    // so a duplicate name is reported as BB_ERR_INVALID_STATE even when the
    // table also happens to be full, matching the precedence documented in
    // bb_diag_section.h.
    if (bb_registry_lookup(&s_bb_diag_section_registry, section->name)) {
        return BB_ERR_INVALID_STATE;
    }

    bb_diag_section_slot_t *slot = find_free_slot();
    if (!slot) return BB_ERR_NO_SPACE;

    strncpy(slot->name, section->name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';

    rc = bb_registry_register(&s_bb_diag_section_registry, slot->name, slot);
    if (rc != BB_OK) return rc;  // LCOV_EXCL_BR_LINE -- the lookup+find_free_slot pair above already proved this name is absent and a slot is free; bb_registry_register() cannot fail here.

    slot->in_use = true;
    slot->section = *section;
    slot->stream_elem_size = stream_elem_size;
    return BB_OK;
}

const bb_diag_section_t *bb_diag_section_find(const char *name)
{
    if (!name) return NULL;

    bb_diag_section_slot_t *slot =
        (bb_diag_section_slot_t *)bb_registry_lookup(&s_bb_diag_section_registry, name);
    return slot ? &slot->section : NULL;
}

size_t bb_diag_section_stream_elem_size(const bb_diag_section_t *sec)
{
    if (!sec) return 0;

    // Recover the owning slot from `sec`'s own address -- every live
    // bb_diag_section_t the caller can hold a pointer to is `&slot->section`
    // for some slot in s_slots (bb_diag_section_find()'s only return path).
    const bb_diag_section_slot_t *slot =
        (const bb_diag_section_slot_t *)((const uint8_t *)sec - offsetof(bb_diag_section_slot_t, section));

    // Defensive: only trust the recovered pointer if it actually lands
    // in-bounds AND on a slot boundary within s_slots[] -- a stray/synthetic
    // bb_diag_section_t* (never returned by bb_diag_section_find()) fails
    // safely (0) instead of reading arbitrary memory as a slot.
    if (slot < s_slots || slot >= s_slots + BB_DIAG_SECTION_TABLE_CAP) return 0;
    if (((const uint8_t *)slot - (const uint8_t *)s_slots) % sizeof(s_slots[0]) != 0) return 0;

    return slot->stream_elem_size;
}

bb_err_t bb_diag_section_name_from_uri(const char *uri, char *out, size_t out_cap)
{
    if (!uri || !out || out_cap == 0) return BB_ERR_INVALID_ARG;

    if (strncmp(uri, BB_DIAG_SECTION_URI_PREFIX, BB_DIAG_SECTION_URI_PREFIX_LEN) != 0) {
        return BB_ERR_NOT_FOUND;
    }

    strncpy(out, uri + BB_DIAG_SECTION_URI_PREFIX_LEN, out_cap - 1);
    out[out_cap - 1] = '\0';
    return BB_OK;
}

bb_err_t bb_diag_section_build_query(const bb_diag_section_t *sec,
                                      bb_diag_query_getter_fn get, void *get_ctx,
                                      char *value_scratch, bb_serialize_query_t *out)
{
    if (!sec || !get || !value_scratch || !out) return BB_ERR_INVALID_ARG;

    out->count = 0;
    for (size_t i = 0; i < sec->n_query_keys; i++) {
        const char *key = sec->query_keys[i];
        char       *value = value_scratch + (i * BB_DIAG_SECTION_QUERY_VALUE_BYTES);

        if (!get(get_ctx, key, value, BB_DIAG_SECTION_QUERY_VALUE_BYTES)) continue;

        out->params[out->count].key   = key;
        out->params[out->count].value = value;
        out->count++;
    }
    return BB_OK;
}

#ifdef BB_DIAG_SECTION_TESTING
void bb_diag_section_test_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    bb_registry_reset(&s_bb_diag_section_registry);
}
#endif
