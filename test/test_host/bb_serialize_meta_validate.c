// bb_serialize_meta_validate — structural agreement check between a
// bb_serialize_desc_t (hot wire descriptor) and its bb_serialize_desc_meta_t
// companion (cold validation+docs metadata). Pure, host-only, no heap.
// First-error semantics: stops at the first violation found and writes a
// human-readable path+key reason.

#include "bb_serialize_meta.h"

#include <stdio.h>
#include <string.h>

// True if `type` (optionally an ARR whose elem_type is checked instead) is
// a string-shaped field -- the only shape min_len/enum_vals are valid for.
static bool is_string_shaped(const bb_serialize_field_t *f)
{
    if (f->type == BB_TYPE_STR || f->type == BB_TYPE_STR_N) return true;
    return f->type == BB_TYPE_ARR && f->elem_type == BB_TYPE_STR;
}

// True if `type` is numeric -- the only shape has_min/has_max are valid for.
static bool is_numeric(bb_type_t type)
{
    return type == BB_TYPE_I64 || type == BB_TYPE_U64 || type == BB_TYPE_F64;
}

static bb_err_t validate_fields(const bb_serialize_field_t *fields, uint16_t n_fields,
                                 const bb_serialize_field_meta_t *rows, uint16_t n_rows,
                                 const char *path, unsigned depth,
                                 char *err, size_t err_len)
{
    // NULL-key guard pass: never dereference a meta row with no key.
    for (uint16_t i = 0; i < n_rows; i++) {
        if (rows[i].key == NULL) {
            snprintf(err, err_len, "%s: meta row %u missing key", path, (unsigned)i);
            return BB_ERR_VALIDATION;
        }
    }

    // Per-field-exactly-one-row pass: every base field must have EXACTLY ONE
    // matching meta row -- 0 is a missing row, >1 is a duplicate. Combined
    // with the orphan-row pass below this is a true bijection between
    // fields and rows. Captures the matched row pointer per field so the
    // constraint pass below doesn't need to re-scan `rows` to find it again.
    const bb_serialize_field_meta_t *matched[n_fields];
    for (uint16_t i = 0; i < n_fields; i++) {
        uint16_t count = 0;
        for (uint16_t j = 0; j < n_rows; j++) {
            if (strcmp(rows[j].key, fields[i].key) == 0) {
                count++;
                matched[i] = &rows[j];
            }
        }
        if (count == 0) {
            snprintf(err, err_len, "%s.%s: missing meta row", path, fields[i].key);
            return BB_ERR_VALIDATION;
        }
        if (count > 1) {
            snprintf(err, err_len, "%s.%s: duplicate meta row for key '%s'",
                     path, fields[i].key, fields[i].key);
            return BB_ERR_VALIDATION;
        }
    }

    // Orphan-row pass: every meta row must have a matching base field.
    for (uint16_t i = 0; i < n_rows; i++) {
        bool found = false;
        for (uint16_t j = 0; j < n_fields; j++) {
            if (strcmp(fields[j].key, rows[i].key) == 0) { found = true; break; }
        }
        if (!found) {
            snprintf(err, err_len, "%s.%s: orphan meta row (no matching field)", path, rows[i].key);
            return BB_ERR_VALIDATION;
        }
    }

    // Per-field constraint agreement + bounds sanity + recursion.
    for (uint16_t i = 0; i < n_fields; i++) {
        const bb_serialize_field_t      *f = &fields[i];
        const bb_serialize_field_meta_t *r = matched[i];

        bool str_shaped = is_string_shaped(f);
        bool numeric     = is_numeric(f->type);

        if ((r->min_len != 0 || r->enum_vals != NULL) && !str_shaped) {
            snprintf(err, err_len,
                     "%s.%s: min_len/enum_vals only valid for string-shaped fields", path, f->key);
            return BB_ERR_VALIDATION;
        }

        if ((r->has_min || r->has_max) && !numeric) {
            snprintf(err, err_len,
                     "%s.%s: min/max only valid for numeric fields", path, f->key);
            return BB_ERR_VALIDATION;
        }

        if (r->has_min && r->has_max && r->min > r->max) {
            snprintf(err, err_len, "%s.%s: min > max", path, f->key);
            return BB_ERR_VALIDATION;
        }

        if (r->min_len != 0 && f->max_len != 0 && r->min_len > f->max_len) {
            snprintf(err, err_len, "%s.%s: min_len exceeds field max_len", path, f->key);
            return BB_ERR_VALIDATION;
        }

        bool nested_obj = f->type == BB_TYPE_OBJ ||
                           (f->type == BB_TYPE_ARR && f->elem_type == BB_TYPE_OBJ);
        if (nested_obj && depth < BB_SERIALIZE_MAX_DEPTH) {
            char child_path[128];
            snprintf(child_path, sizeof(child_path), "%s.%s", path, f->key);
            bb_err_t rc = validate_fields(f->children, f->n_children,
                                           r->children, r->n_children,
                                           child_path, depth + 1, err, err_len);
            if (rc != BB_OK) return rc;
        }
    }

    return BB_OK;
}

bb_err_t bb_serialize_meta_validate(const bb_serialize_desc_t      *desc,
                                     const bb_serialize_desc_meta_t *meta,
                                     char *err, size_t err_len)
{
    if (strcmp(desc->type_name, meta->type_name) != 0) {
        snprintf(err, err_len, "type_name mismatch: desc '%s' vs meta '%s'",
                 desc->type_name, meta->type_name);
        return BB_ERR_VALIDATION;
    }

    return validate_fields(desc->fields, desc->n_fields, meta->rows, meta->n_rows,
                            desc->type_name, 0, err, err_len);
}
