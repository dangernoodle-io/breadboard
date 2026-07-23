// Pure descriptor walker -- no heap, no locks, no I/O, no format knowledge.
// Drives an arbitrary bb_serialize_emit_t against a snapshot struct
// described by a bb_serialize_desc_t.

#include "bb_serialize.h"

#include <string.h>

static void walk_fields(const bb_serialize_field_t *fields, uint16_t n_fields,
                         const void *snap, const bb_serialize_emit_t *emit, unsigned depth,
                         bb_serialize_ref_resolve_fn resolve, void *resolve_ctx)
{
    for (uint16_t i = 0; i < n_fields; i++) {
        const bb_serialize_field_t *f = &fields[i];
        if (f->present && !f->present(snap)) continue;

        const uint8_t *p = (const uint8_t *)snap + f->offset;

        switch (f->type) {
        case BB_TYPE_I64: {
            int64_t v;
            memcpy(&v, p, sizeof(v));
            emit->emit_i64(emit->ctx, f->key, v);
            break;
        }
        case BB_TYPE_U64: {
            uint64_t v;
            memcpy(&v, p, sizeof(v));
            emit->emit_u64(emit->ctx, f->key, v);
            break;
        }
        case BB_TYPE_F64: {
            double v;
            memcpy(&v, p, sizeof(v));
            emit->emit_f64(emit->ctx, f->key, v);
            break;
        }
        case BB_TYPE_BOOL: {
            bool v;
            memcpy(&v, p, sizeof(v));
            emit->emit_bool(emit->ctx, f->key, v);
            break;
        }
        case BB_TYPE_STR: {
            // The field IS a NUL-terminated char buffer starting at offset
            // (a snapshot struct's embedded char[N] member) -- read
            // directly, no pointer indirection. Bounded by f->max_len via
            // strnlen(), NEVER strlen(): a buffer that isn't NUL-terminated
            // within its own array bound (network-filled/corrupt/racy
            // snapshot) yields exactly max_len bytes rather than an OOB
            // read past the array end.
            const char *s = (const char *)p;
            size_t n = strnlen(s, f->max_len);
            emit->emit_str(emit->ctx, f->key, s, n);
            break;
        }
        case BB_TYPE_STR_N: {
            // The length is explicit and already safe -- no strlen. A NULL
            // ptr is a genuine null value (emit_null), distinct from a
            // non-NULL empty string (.len == 0, emit_str).
            bb_serialize_str_n_t sn;
            memcpy(&sn, p, sizeof(sn));
            if (sn.ptr) {
                emit->emit_str(emit->ctx, f->key, sn.ptr, sn.len);
            } else {
                emit->emit_null(emit->ctx, f->key);
            }
            break;
        }
        case BB_TYPE_OBJ: {
            // Defensive against a copy-paste circular `children` pointer:
            // bail rather than recurse past BB_SERIALIZE_MAX_DEPTH.
            if (depth >= BB_SERIALIZE_MAX_DEPTH) break;
            emit->begin_obj(emit->ctx, f->key);
            walk_fields(f->children, f->n_children, p, emit, depth + 1, resolve, resolve_ctx);
            emit->end_obj(emit->ctx);
            break;
        }
        case BB_TYPE_ARR: {
            emit->begin_arr(emit->ctx, f->key);
            if (f->cardinality == BB_ARR_STREAM) {
                // Pull iterator, not a pre-materialized contiguous array --
                // see bb_serialize.h's bb_serialize_arr_stream_t doc.
                // reg-time validation (bb_diag_register_section()) already
                // enforces elem_type == BB_TYPE_OBJ for a STREAM field; the
                // walker trusts the descriptor here, same as every other
                // shape below.
                bb_serialize_arr_stream_t carrier;
                memcpy(&carrier, p, sizeof(carrier));
                if (carrier.next && depth < BB_SERIALIZE_MAX_DEPTH) {
                    uint8_t row_buf[BB_SERIALIZE_MAX_ROW_BYTES];  // one buffer per STREAM field, reused per row
                    while (carrier.next(carrier.iter_ctx, row_buf)) {
                        emit->begin_obj(emit->ctx, NULL);
                        walk_fields(f->children, f->n_children, row_buf, emit, depth + 1,
                                    resolve, resolve_ctx);
                        emit->end_obj(emit->ctx);
                    }
                }
            } else {
                // BB_ARR_FIXED (the zero-init default) -- byte-identical to
                // the walker's original behavior, untouched.
                bb_serialize_arr_t arr;
                memcpy(&arr, p, sizeof(arr));
                if (arr.items) {
                    // NULL items with count > 0 is caller UB -- degrade to an
                    // empty array (begin_arr/end_arr only) rather than deref.
                    if (f->elem_type == BB_TYPE_OBJ) {
                        if (depth < BB_SERIALIZE_MAX_DEPTH) {
                            const uint8_t *base = (const uint8_t *)arr.items;
                            for (size_t j = 0; j < arr.count; j++) {
                                const uint8_t *elem = base + j * f->elem_size;
                                emit->begin_obj(emit->ctx, NULL);
                                walk_fields(f->children, f->n_children, elem, emit, depth + 1,
                                            resolve, resolve_ctx);
                                emit->end_obj(emit->ctx);
                            }
                        }
                    } else if (f->elem_type == BB_TYPE_STR) {
                        // elem_type == BB_TYPE_STR: items is const char *const *.
                        // Bounded the same way as the embedded-STR path: NEVER
                        // strlen -- an element that isn't NUL-terminated within
                        // f->max_len yields exactly max_len bytes, not an OOB
                        // read past the caller's buffer.
                        const char *const *items = (const char *const *)arr.items;
                        for (size_t j = 0; j < arr.count; j++) {
                            const char *s = items[j];
                            if (s) {
                                size_t n = strnlen(s, f->max_len);
                                emit->emit_str(emit->ctx, NULL, s, n);
                            } else {
                                emit->emit_null(emit->ctx, NULL);
                            }
                        }
                    } else {
                        // Scalar elem_type (I64/U64/F64/BOOL): items is a
                        // contiguous array of the element's own underlying
                        // C type -- no elem_size needed (unlike OBJ), since a
                        // scalar's stride is fully determined by its type.
                        // NULL key per element, same convention as the STR
                        // item branch above.
                        switch (f->elem_type) {
                        case BB_TYPE_I64: {
                            const int64_t *items = (const int64_t *)arr.items;
                            for (size_t j = 0; j < arr.count; j++) {
                                emit->emit_i64(emit->ctx, NULL, items[j]);
                            }
                            break;
                        }
                        case BB_TYPE_U64: {
                            const uint64_t *items = (const uint64_t *)arr.items;
                            for (size_t j = 0; j < arr.count; j++) {
                                emit->emit_u64(emit->ctx, NULL, items[j]);
                            }
                            break;
                        }
                        case BB_TYPE_F64: {
                            const double *items = (const double *)arr.items;
                            for (size_t j = 0; j < arr.count; j++) {
                                emit->emit_f64(emit->ctx, NULL, items[j]);
                            }
                            break;
                        }
                        case BB_TYPE_BOOL: {
                            const bool *items = (const bool *)arr.items;
                            for (size_t j = 0; j < arr.count; j++) {
                                emit->emit_bool(emit->ctx, NULL, items[j]);
                            }
                            break;
                        }
                        default: break;  // LCOV_EXCL_LINE -- unsupported elem_type, defensive
                        }
                    }
                }
            }
            emit->end_arr(emit->ctx);
            break;
        }
        case BB_TYPE_REF: {
            // A REF hop costs one depth level, same accounting as OBJ --
            // bounded defense against a resolver cycle (A -> B -> A ...)
            // rather than a real-world limit: frames are bounded (<= 8),
            // the composition data driving them is static-const/trusted
            // (same precedent as the OBJ depth guard above), and a cycle
            // truncates deterministically at the cap rather than
            // stack-overflowing.
            if (depth >= BB_SERIALIZE_MAX_DEPTH) break;

            bb_serialize_ref_t ref = { .desc = NULL, .snap = NULL };
            if (!resolve || !resolve(f->ref_key, resolve_ctx, &ref) || !ref.desc || !ref.snap) {
                // No resolver, resolver reports no such sibling, or a
                // resolver returns true but leaves desc/snap unset (buggy
                // resolver / future cache-race): omit the field entirely --
                // same convention as `present` == false, never a crash.
                break;
            }

            emit->begin_obj(emit->ctx, f->key);
            walk_fields(ref.desc->fields, ref.desc->n_fields, ref.snap, emit, depth + 1,
                        resolve, resolve_ctx);
            emit->end_obj(emit->ctx);
            break;
        }
        default:
            break;  // LCOV_EXCL_LINE -- exhaustive enum, defensive
        }
    }
}

void bb_serialize_walk(const bb_serialize_desc_t *desc, const void *snap,
                        const bb_serialize_emit_t *emit)
{
    bb_serialize_walk_ref(desc, snap, emit, NULL, NULL);
}

void bb_serialize_walk_ref(const bb_serialize_desc_t *desc, const void *snap,
                            const bb_serialize_emit_t *emit,
                            bb_serialize_ref_resolve_fn resolve, void *resolve_ctx)
{
    if (!desc || !snap || !emit) return;  // LCOV_EXCL_BR_LINE -- defensive against NULL misuse
    walk_fields(desc->fields, desc->n_fields, snap, emit, 0, resolve, resolve_ctx);
}

const bb_serialize_field_t *bb_serialize_desc_find(const bb_serialize_desc_t *desc,
                                                    const char *key)
{
    if (!desc || !key) return NULL;
    for (uint16_t i = 0; i < desc->n_fields; i++) {
        if (desc->fields[i].key && strcmp(desc->fields[i].key, key) == 0) {
            return &desc->fields[i];
        }
    }
    return NULL;
}
