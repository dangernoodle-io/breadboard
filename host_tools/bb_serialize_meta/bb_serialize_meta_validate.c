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

    // Key-level row-matching pass: every DISTINCT key in `fields` must have
    // EXACTLY ONE matching meta row -- 0 is a missing row, >1 is a
    // duplicate. Combined with the orphan-row pass below this is a true
    // bijection between KEYS and rows (B1-1181a relaxes the PHYSICAL-field
    // bijection the comment above once described -- a duplicate-`.key`
    // group is now legitimate, see bb_serialize_field_meta_s's
    // "duplicate-key" doc). Captures, per physical field, the meta row that
    // documents THAT SPECIFIC occurrence (NULL for an occurrence
    // intentionally left undocumented, e.g. reboot's F64 divergence-guard
    // shadow) so the constraint pass below doesn't need to re-derive it.
    const bb_serialize_field_meta_t *matched[n_fields];
    for (uint16_t i = 0; i < n_fields; i++) matched[i] = NULL;

    for (uint16_t i = 0; i < n_fields; i++) {
        if (bb_serialize_meta_occurrence_index(fields, i) != 0) continue;  // process each key once

        const char *key       = fields[i].key;
        uint16_t    occ_count = bb_serialize_meta_occurrence_count(fields, n_fields, key);

        // Contiguity guard (B1-1181a review finding): the ONEOF-branch
        // matching (branches[k] <-> fields[i+k]) and the occurrence-tagged
        // FIELD path (matched[i + row->occurrence]) both assume every
        // physical occurrence of a duplicate key appears back-to-back
        // starting at `i` (see bb_serialize_field_meta_s's "duplicate-key"
        // doc). An interleaved table (e.g. [X, Y, X]) would otherwise
        // silently misattribute a row to the wrong physical field and
        // still return BB_OK -- the exact silent-pass failure mode this
        // relaxed bijection must not allow. Checked BEFORE any row lookup:
        // it's a desc-level structural defect, independent of the meta
        // table.
        if (occ_count > 1) {
            for (uint16_t j = i; j < i + occ_count; j++) {
                if (j >= n_fields || strcmp(fields[j].key, key) != 0) {
                    snprintf(err, err_len, "%s.%s: non-contiguous duplicate key", path, key);
                    return BB_ERR_VALIDATION;
                }
            }
        }

        uint16_t                         count = 0;
        const bb_serialize_field_meta_t *found = NULL;
        for (uint16_t j = 0; j < n_rows; j++) {
            if (strcmp(rows[j].key, key) == 0) {
                count++;
                found = &rows[j];
            }
        }

        if (count == 0) {
            if (occ_count == 1) {
                snprintf(err, err_len, "%s.%s: missing meta row", path, key);
            } else {
                snprintf(err, err_len,
                         "%s.%s: duplicate key with no oneOf/occurrence annotation", path, key);
            }
            return BB_ERR_VALIDATION;
        }
        if (count > 1) {
            snprintf(err, err_len, "%s.%s: duplicate meta row for key '%s'", path, key, key);
            return BB_ERR_VALIDATION;
        }

        const bb_serialize_field_meta_t *row = found;

        if (occ_count == 1 && row->kind == BB_SERIALIZE_META_KIND_FIELD) {
            // Ordinary, non-duplicated key -- the plain 1:1 bijection.
            matched[i] = row;
            continue;
        }

        if (row->kind == BB_SERIALIZE_META_KIND_ONEOF) {
            if (row->n_branches != occ_count) {
                snprintf(err, err_len, "%s.%s: oneOf n_branches (%u) != occurrence count (%u)",
                         path, key, (unsigned)row->n_branches, (unsigned)occ_count);
                return BB_ERR_VALIDATION;
            }
            if (!row->branches) {
                snprintf(err, err_len, "%s.%s: oneOf row missing branches", path, key);
                return BB_ERR_VALIDATION;
            }
            for (uint16_t k = 0; k < row->n_branches; k++) {
                const bb_serialize_field_meta_t *branch = row->branches[k];
                if (!branch) {
                    snprintf(err, err_len, "%s.%s: oneOf branch %u is NULL", path, key, (unsigned)k);
                    return BB_ERR_VALIDATION;
                }
                if (branch->key == NULL || strcmp(branch->key, key) != 0) {
                    snprintf(err, err_len, "%s.%s: orphan oneOf branch %u (key mismatch)",
                             path, key, (unsigned)k);
                    return BB_ERR_VALIDATION;
                }
                matched[i + k] = branch;
            }
            continue;
        }

        // row->kind == BB_SERIALIZE_META_KIND_FIELD, occ_count > 1 (or an
        // ONEOF row was mistakenly used on a non-duplicated key -- handled
        // by the n_branches != occ_count check above for occ_count == 1).
        if (row->occurrence >= occ_count) {
            snprintf(err, err_len, "%s.%s: occurrence %u out of range (count %u)",
                     path, key, (unsigned)row->occurrence, (unsigned)occ_count);
            return BB_ERR_VALIDATION;
        }
        matched[i + row->occurrence] = row;
        // Every other physical occurrence of `key` is intentionally left
        // undocumented (matched[] stays NULL) -- the reboot "ts" shape.
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

    // Per-field constraint agreement + bounds sanity + recursion. A field
    // whose physical occurrence was intentionally left undocumented
    // (matched[i] == NULL -- an occurrence-tagged FIELD row's untagged
    // sibling occurrence, e.g. reboot's F64 divergence-guard shadow) has no
    // constraints to check and no recursion to perform.
    for (uint16_t i = 0; i < n_fields; i++) {
        const bb_serialize_field_t      *f = &fields[i];
        const bb_serialize_field_meta_t *r = matched[i];
        if (!r) continue;

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
