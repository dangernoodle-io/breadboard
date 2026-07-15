// Pure descriptor scatterer -- no heap, no locks, no I/O, no format
// knowledge. The pull-direction inverse of bb_serialize_walk.c: drives an
// arbitrary bb_serialize_populate_t against the SAME descriptor tables a
// walk() call would emit from, writing values INTO a snapshot struct
// instead of reading them out of one.

#include "bb_serialize.h"

#include <string.h>

// Pre-flight capability check -- scans the descriptor (never the source or
// dst) so an unsupported/misconfigured field is rejected LOUD before any
// scatter begins, avoiding a partial write into caller memory. Depth-capped
// at BB_SERIALIZE_MAX_DEPTH to match populate_fields()'s own recursion
// bound -- a self-referential descriptor table (children pointing back at
// itself) would otherwise recurse forever here even though no source data
// is involved; once the cap is hit, scanning simply stops (the runtime
// depth guard in populate_fields() is what reports BB_ERR_NO_SPACE for that
// case).
static bb_err_t populate_check_fields(const bb_serialize_field_t *fields, uint16_t n_fields,
                                       unsigned depth)
{
    if (depth >= BB_SERIALIZE_MAX_DEPTH) return BB_OK;

    for (uint16_t i = 0; i < n_fields; i++) {
        const bb_serialize_field_t *f = &fields[i];

        switch (f->type) {
        case BB_TYPE_STR_N:
        case BB_TYPE_REF:
            // Not supported by populate -- both target caller-owned storage
            // outside the snapshot struct itself (a STR_N's `.ptr`, a REF's
            // resolved sibling) with no settled scatter convention yet.
            return BB_ERR_UNSUPPORTED;
        case BB_TYPE_OBJ: {
            bb_err_t rc = populate_check_fields(f->children, f->n_children, depth + 1);
            if (rc != BB_OK) return rc;
            break;
        }
        case BB_TYPE_ARR:
            // max_items is populate's destination CAPACITY -- 0 means "no
            // storage to write into", which populate treats as a loud
            // misconfiguration rather than JSON's 0-means-unbounded.
            if (f->max_items == 0) return BB_ERR_INVALID_ARG;
            if (f->elem_type == BB_TYPE_OBJ) {
                bb_err_t rc = populate_check_fields(f->children, f->n_children, depth + 1);
                if (rc != BB_OK) return rc;
            }
            break;
        default:
            break;
        }
    }
    return BB_OK;
}

static bb_err_t populate_fields(const bb_serialize_field_t *fields, uint16_t n_fields,
                                 void *dst, const bb_serialize_populate_t *src, unsigned depth)
{
    for (uint16_t i = 0; i < n_fields; i++) {
        const bb_serialize_field_t *f = &fields[i];
        uint8_t *p = (uint8_t *)dst + f->offset;

        // The STR_N/REF arm below is unreachable by design: populate_check_fields()
        // rejects any descriptor containing either before populate_fields() ever runs.
        switch (f->type) {  // LCOV_EXCL_BR_LINE
        case BB_TYPE_I64: {
            int64_t v;
            if (src->get_i64(src->ctx, f->key, &v)) memcpy(p, &v, sizeof(v));
            break;
        }
        case BB_TYPE_U64: {
            uint64_t v;
            if (src->get_u64(src->ctx, f->key, &v)) memcpy(p, &v, sizeof(v));
            break;
        }
        case BB_TYPE_F64: {
            double v;
            if (src->get_f64(src->ctx, f->key, &v)) memcpy(p, &v, sizeof(v));
            break;
        }
        case BB_TYPE_BOOL: {
            bool v;
            if (src->get_bool(src->ctx, f->key, &v)) memcpy(p, &v, sizeof(v));
            break;
        }
        case BB_TYPE_STR: {
            // The field IS the embedded char[N] buffer at `offset` -- write
            // directly, bounded by f->max_len (the getter's own contract to
            // honor, populate never re-validates it). A false return means
            // absent -- the getter contract requires it leave `dst`
            // untouched in that case, same as every other getter.
            size_t out_len;
            // Both arms fall through to the same `break` -- checking the
            // return here is contract documentation, not a functional guard
            // (unlike the scalar getters' scratch-temp copy or the array-STR
            // loop's continuation control). `dst` staying untouched on false
            // is the getter's responsibility, not enforced by this branch.
            if (!src->get_str(src->ctx, f->key, (char *)p, f->max_len, &out_len)) break;
            break;
        }
        case BB_TYPE_STR_N:   // LCOV_EXCL_LINE -- pre-flight-rejected, defensive
        case BB_TYPE_REF:     // LCOV_EXCL_LINE -- pre-flight-rejected, defensive
            // Unreachable: populate_check_fields() rejects a descriptor
            // containing either before any scatter begins.
            break;  // LCOV_EXCL_LINE -- pre-flight-rejected, defensive
        case BB_TYPE_OBJ: {
            // Defensive against a copy-paste circular `children` pointer,
            // same guard as bb_serialize_walk() -- but populate reports it
            // (a silent partial scatter into caller memory is worse than a
            // walker's read-only truncation).
            if (depth >= BB_SERIALIZE_MAX_DEPTH) return BB_ERR_NO_SPACE;
            if (src->begin_obj(src->ctx, f->key)) {
                bb_err_t rc = populate_fields(f->children, f->n_children, p, src, depth + 1);
                src->end_obj(src->ctx);
                if (rc != BB_OK) return rc;
            }
            break;
        }
        case BB_TYPE_ARR: {
            size_t count = 0;
            if (!src->begin_arr(src->ctx, f->key, &count)) break;  // absent: leave untouched

            bb_serialize_arr_t arr;
            memcpy(&arr, p, sizeof(arr));  // caller-prewired `.items`; `.count` is overwritten below
            size_t written = 0;

            if (arr.items) {
                // NULL items is caller UB (nothing pre-wired to write
                // into) -- degrade to zero elements rather than deref, the
                // same convention as bb_serialize_walk()'s NULL-items guard.
                size_t cap = f->max_items;
                size_t n = count < cap ? count : cap;

                if (f->elem_type == BB_TYPE_OBJ) {
                    if (depth >= BB_SERIALIZE_MAX_DEPTH) {
                        src->end_arr(src->ctx);
                        return BB_ERR_NO_SPACE;
                    }
                    uint8_t *base = (uint8_t *)arr.items;
                    for (; written < n; written++) {
                        uint8_t *elem = base + written * f->elem_size;
                        if (!src->begin_obj(src->ctx, NULL)) break;
                        bb_err_t rc = populate_fields(f->children, f->n_children, elem, src, depth + 1);
                        src->end_obj(src->ctx);
                        if (rc != BB_OK) {
                            src->end_arr(src->ctx);
                            return rc;
                        }
                    }
                } else {
                    // elem_type == BB_TYPE_STR: `.items` is a pre-wired
                    // array of writable `char *` buffers, each >= max_len.
                    char *const *items = (char *const *)arr.items;
                    for (; written < n; written++) {
                        size_t out_len;
                        if (!src->get_str(src->ctx, NULL, items[written], f->max_len, &out_len)) break;
                    }
                }
            }

            arr.count = written;
            memcpy(p, &arr, sizeof(arr));
            src->end_arr(src->ctx);
            break;
        }
        default:
            break;  // LCOV_EXCL_LINE -- exhaustive enum, defensive
        }
    }
    return BB_OK;
}

bb_err_t bb_serialize_populate(const bb_serialize_desc_t *desc, void *dst,
                                const bb_serialize_populate_t *src)
{
    if (!desc || !dst || !src) return BB_ERR_INVALID_ARG;

    bb_err_t rc = populate_check_fields(desc->fields, desc->n_fields, 0);
    if (rc != BB_OK) return rc;

    return populate_fields(desc->fields, desc->n_fields, dst, src, 0);
}
