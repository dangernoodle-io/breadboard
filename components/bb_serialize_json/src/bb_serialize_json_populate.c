// Populate adapter -- binds a bb_serialize_populate_t source vtable to the
// root object of an already-scanned bb_serialize_json_tok_recorder_t, so a
// caller can drive bb_serialize_populate() straight off a scanned document.
// See bb_serialize_json_populate_from_tok()'s doc comment in
// bb_serialize_json.h for the full contract (in particular the
// RE-DRIVABLE guarantee this file's design depends on).

#include "bb_serialize_json.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Internal cursor layout -- cast onto bb_serialize_json_populate_ctx_t's
// opaque `_state[]`, same pattern as bb_serialize_json_scan_ctx_t.
// ---------------------------------------------------------------------------

// One level of the traversal stack: the container token (OBJ or ARR)
// currently in scope, plus the next unread element index -- only consumed
// for an ARR frame (an OBJ frame's members are always looked up by key, so
// `next_elem` simply stays 0 and unused for it).
typedef struct {
    bb_serialize_json_tok_idx_t idx;
    size_t                      next_elem;
} populate_frame_t;

typedef struct {
    const bb_serialize_json_tok_recorder_t *rec;
    populate_frame_t                        stack[BB_SERIALIZE_MAX_DEPTH + 1];
    uint8_t                                 depth;  // index of the current (top) frame
} populate_state_t;

_Static_assert(sizeof(populate_state_t) <= BB_SERIALIZE_JSON_POPULATE_CTX_STATE_SIZE,
                "BB_SERIALIZE_JSON_POPULATE_CTX_STATE_SIZE too small for populate_state_t");

static populate_state_t *state_of(void *ctx)
{
    return (populate_state_t *)((bb_serialize_json_populate_ctx_t *)ctx)->_state;
}

static populate_frame_t *top_of(populate_state_t *st)
{
    return &st->stack[st->depth];
}

// ---------------------------------------------------------------------------
// bb_serialize_populate_t callbacks
// ---------------------------------------------------------------------------
// Scalar getters (get_i64/get_u64/get_f64/get_bool) are only ever called by
// bb_serialize_populate() with a non-NULL `key` -- a plain (non-array,
// non-object) field of the current OBJ frame. get_str is the one getter
// populate also calls with `key` == NULL, for a BB_TYPE_ARR element whose
// elem_type == BB_TYPE_STR; begin_obj is the one container callback
// populate also calls with `key` == NULL, for a BB_TYPE_ARR element whose
// elem_type == BB_TYPE_OBJ. Both NULL-key cases consume the current frame's
// `next_elem` cursor.

static bool populate_get_i64(void *ctx, const char *key, int64_t *out)
{
    populate_state_t *st = state_of(ctx);
    bb_serialize_json_tok_idx_t tok =
        bb_serialize_json_tok_obj_get(st->rec, top_of(st)->idx, key, strlen(key));
    return bb_serialize_json_tok_get_i64(st->rec, tok, out);
}

static bool populate_get_u64(void *ctx, const char *key, uint64_t *out)
{
    populate_state_t *st = state_of(ctx);
    bb_serialize_json_tok_idx_t tok =
        bb_serialize_json_tok_obj_get(st->rec, top_of(st)->idx, key, strlen(key));
    int64_t v;
    if (!bb_serialize_json_tok_get_i64(st->rec, tok, &v)) return false;
    *out = (uint64_t)v;
    return true;
}

static bool populate_get_f64(void *ctx, const char *key, double *out)
{
    populate_state_t *st = state_of(ctx);
    bb_serialize_json_tok_idx_t tok =
        bb_serialize_json_tok_obj_get(st->rec, top_of(st)->idx, key, strlen(key));
    return bb_serialize_json_tok_get_f64(st->rec, tok, out);
}

static bool populate_get_bool(void *ctx, const char *key, bool *out)
{
    populate_state_t *st = state_of(ctx);
    bb_serialize_json_tok_idx_t tok =
        bb_serialize_json_tok_obj_get(st->rec, top_of(st)->idx, key, strlen(key));
    return bb_serialize_json_tok_get_bool(st->rec, tok, out);
}

static bool populate_get_str(void *ctx, const char *key, char *dst, size_t cap, size_t *out_len)
{
    populate_state_t *st  = state_of(ctx);
    populate_frame_t  *top = top_of(st);
    bb_serialize_json_tok_idx_t tok;

    if (key) {
        tok = bb_serialize_json_tok_obj_get(st->rec, top->idx, key, strlen(key));
    } else {
        // BB_TYPE_ARR element, elem_type == BB_TYPE_STR: consume the next
        // unread element of the current ARR frame.
        tok = bb_serialize_json_tok_arr_at(st->rec, top->idx, top->next_elem);
        top->next_elem++;
    }

    const char *ptr;
    size_t      len;
    if (!bb_serialize_json_tok_get_str(st->rec, tok, &ptr, &len)) return false;

    // Bounded write into `dst`, capacity `cap` (the field's max_len) -- the
    // getter's own bounds-checking responsibility per the populate
    // contract (see bb_serialize_populate_t.get_str's doc in
    // bb_serialize.h). Never NUL-terminates: `dst` is the destination
    // BB_TYPE_STR embedded buffer, caller-zeroed per populate's contract,
    // so any bytes beyond the written prefix are already zero. memcpy with
    // n == 0 (an empty string value) is well-defined -- no guard needed.
    size_t n = len < cap ? len : cap;
    memcpy(dst, ptr, n);
    *out_len = n;
    return true;
}

static bool populate_begin_obj(void *ctx, const char *key)
{
    populate_state_t *st  = state_of(ctx);
    populate_frame_t  *top = top_of(st);
    bb_serialize_json_tok_idx_t tok;

    if (key) {
        tok = bb_serialize_json_tok_obj_get(st->rec, top->idx, key, strlen(key));
    } else {
        // BB_TYPE_ARR element, elem_type == BB_TYPE_OBJ: consume the next
        // unread element of the current ARR frame.
        tok = bb_serialize_json_tok_arr_at(st->rec, top->idx, top->next_elem);
        top->next_elem++;
    }

    if (!bb_serialize_json_tok_is_obj(st->rec, tok)) return false;

    // `stack[]` holds BB_SERIALIZE_MAX_DEPTH + 1 frames (valid indices
    // 0..BB_SERIALIZE_MAX_DEPTH) -- st->depth legitimately reaches
    // BB_SERIALIZE_MAX_DEPTH itself (a chain of BB_SERIALIZE_MAX_DEPTH
    // nested begin_obj calls lands the top frame at that last valid
    // index). bb_serialize_populate() guards BB_TYPE_OBJ nesting against
    // that same bound before calling us, so in practice st->depth is
    // already < BB_SERIALIZE_MAX_DEPTH here -- but this adapter stays
    // self-safe even if that core guard is ever missing or wrong: fail
    // closed rather than push past the last valid frame.
    if (st->depth >= BB_SERIALIZE_MAX_DEPTH) return false;
    st->depth++;
    populate_frame_t *pushed = top_of(st);
    pushed->idx       = tok;
    pushed->next_elem = 0;
    return true;
}

static bool populate_end_obj(void *ctx)
{
    populate_state_t *st = state_of(ctx);
    // bb_serialize_populate() always pairs end_obj with a prior successful
    // begin_obj, so depth == 0 here is unreachable in practice -- defensive.
    if (st->depth == 0) return false;  // LCOV_EXCL_LINE -- unmatched end_obj never occurs
    st->depth--;
    return true;
}

static bool populate_begin_arr(void *ctx, const char *key, size_t *count)
{
    populate_state_t *st = state_of(ctx);
    bb_serialize_json_tok_idx_t tok =
        bb_serialize_json_tok_obj_get(st->rec, top_of(st)->idx, key, strlen(key));

    if (!bb_serialize_json_tok_is_arr(st->rec, tok)) return false;

    // `stack[]` holds BB_SERIALIZE_MAX_DEPTH + 1 frames (valid indices
    // 0..BB_SERIALIZE_MAX_DEPTH) -- st->depth legitimately reaches
    // BB_SERIALIZE_MAX_DEPTH itself, same as begin_obj above. As of core
    // fix #905, bb_serialize_populate() now depth-gates BB_TYPE_ARR
    // nesting against that same bound before calling us too, so in
    // practice st->depth is already < BB_SERIALIZE_MAX_DEPTH here -- but
    // this adapter stays self-safe even if that core guard is ever
    // missing or wrong: fail closed rather than push past the last valid
    // frame. Kept live via a white-box test (not LCOV_EXCL_LINE'd) as
    // defense-in-depth against a future core regression reintroducing
    // the pre-#905 gap. Checked before the `*count` write below so a
    // fail-closed rejection writes nothing to the caller's output.
    if (st->depth >= BB_SERIALIZE_MAX_DEPTH) return false;

    // bb_serialize_populate() always passes a non-NULL `count` -- no guard
    // needed.
    *count = (size_t)bb_serialize_json_tok_arr_size(st->rec, tok);

    st->depth++;
    populate_frame_t *pushed = top_of(st);
    pushed->idx       = tok;
    pushed->next_elem = 0;
    return true;
}

static bool populate_end_arr(void *ctx)
{
    populate_state_t *st = state_of(ctx);
    // bb_serialize_populate() always pairs end_arr with a prior successful
    // begin_arr, so depth == 0 here is unreachable in practice -- defensive.
    if (st->depth == 0) return false;  // LCOV_EXCL_LINE -- unmatched end_arr never occurs
    st->depth--;
    return true;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

bb_serialize_populate_t bb_serialize_json_populate_from_tok(
    bb_serialize_json_populate_ctx_t *ctx,
    const bb_serialize_json_tok_recorder_t *rec)
{
    populate_state_t *st = state_of(ctx);
    st->rec   = rec;
    st->depth = 0;
    st->stack[0].idx       = bb_serialize_json_tok_root(rec);
    st->stack[0].next_elem = 0;

    bb_serialize_populate_t src = {
        .format_id = BB_FORMAT_JSON,
        .ctx       = ctx,
        .get_i64   = populate_get_i64,
        .get_u64   = populate_get_u64,
        .get_f64   = populate_get_f64,
        .get_bool  = populate_get_bool,
        .get_str   = populate_get_str,
        .begin_obj = populate_begin_obj,
        .end_obj   = populate_end_obj,
        .begin_arr = populate_begin_arr,
        .end_arr   = populate_end_arr,
    };
    return src;
}
