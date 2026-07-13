// Token recorder -- a bb_serialize_json_ingest_t sink that records a
// bb_serialize_json_scan_bounded() document into a flat, caller-owned pool
// of bb_serialize_json_tok_t, plus random-access navigation/accessors.
// BOUNDED-MODE ONLY -- see the file banner in bb_serialize_json.h.
//
// Pool layout: tokens are appended in pre-order (document) scan order, and
// every token stores its `parent` index. Object-key / array-index lookups
// are a linear scan from the container's own index, counting only tokens
// whose `parent` matches the container -- descendants of an earlier child
// are skipped automatically (they have a different, deeper parent), and the
// scan stops as soon as the container's recorded `child_count` direct
// children have all been seen. This mirrors the classic flat-token-array
// (jsmn-style) parser shape.

#include "bb_serialize_json.h"

#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Kconfig bridge -- CONFIG_BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES -> a C
// default. Never shadow the generated symbol with a bare #ifndef. Reused
// here (not a new Kconfig option) purely to size the local buffer used to
// NUL-terminate a number's raw text for strtoll()/strtod() -- the scanner
// itself enforces that a number's text can never exceed this many bytes
// (see bb_serialize_json_scan.c's scratch_append()), so a same-sized local
// buffer always fits it.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES
#define BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES CONFIG_BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES
#else
#define BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES 64
#endif

// Enforces the documented bb_serialize_json_tok_t size (see its doc comment
// in bb_serialize_json.h) -- nothing else catches this drifting, since the
// struct has no other size-sensitive consumer. Verified equal (not just
// bounded) on both a 64-bit host build and 32-bit ARM.
_Static_assert(sizeof(bb_serialize_json_tok_t) == 48,
                "bb_serialize_json_tok_t size drifted from its documented 48 bytes -- update the doc comment in bb_serialize_json.h too");

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool tok_valid(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx)
{
    return rec != NULL && idx >= 0 && (size_t)idx < rec->pool_n;
}

// Bump-allocates a slot from `rec->pool`. Considered and rejected reusing
// `bb_pool` (components/bb_pool, TRANSIENT mode) for this: it's a generic
// byte-bump allocator with no notion of a typed slot array or parent/child
// linking, so wrapping it here would need an arena adapter plus a heavier
// API on top for what is a small, domain-specific allocation shape --
// not a good fit.
//
// Allocates a new token, copies `key`/`key_len` into its inline key buffer,
// links it to the current top-of-stack container (or the root sentinel if
// the stack is empty), and bumps that parent's child_count. Returns the new
// token's index, or -1 on pool exhaustion, an oversized key, or a parent
// container's child_count already at its UINT16_MAX ceiling -- all three are
// BB_ERR_NO_SPACE conditions to the caller.
static bb_serialize_json_tok_idx_t tok_alloc(bb_serialize_json_tok_recorder_t *rec,
                                              bb_serialize_json_tok_type_t type,
                                              const char *key, size_t key_len)
{
    if (key_len > BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN) return -1;
    if (rec->pool_n >= rec->pool_cap) return -1;
    // child_count is uint16_t; check BEFORE consuming a pool slot below --
    // a container with UINT16_MAX direct children already at the increment
    // boundary must fail rather than silently wrap to 0 and corrupt
    // obj_get()/arr_size()/arr_at() navigation with no error signal.
    if (rec->depth > 0 && rec->pool[rec->stack[rec->depth - 1]].child_count == UINT16_MAX) return -1;

    bb_serialize_json_tok_idx_t idx = (bb_serialize_json_tok_idx_t)rec->pool_n++;
    bb_serialize_json_tok_t    *t = &rec->pool[idx];
    memset(t, 0, sizeof(*t));
    t->type = (uint8_t)type;
    // `key_len > 0` is defensive, not reachable independently of `key !=
    // NULL`: the scanner's own cur_key() (bb_serialize_json_scan.c) returns
    // NULL exactly when key_len == 0 (an object member can never have a
    // truly empty-string key name distinguishable from "no key" in this
    // callback contract), so key_len is always > 0 whenever key is non-NULL.
    if (key != NULL && key_len > 0) {  // LCOV_EXCL_BR_LINE -- key_len>0 false-with-key!=NULL is unreachable, see above
        memcpy(t->key, key, key_len);
        t->key_len = (uint8_t)key_len;
    }

    if (rec->depth > 0) {
        bb_serialize_json_tok_idx_t parent = rec->stack[rec->depth - 1];
        t->parent = parent;
        rec->pool[parent].child_count++;
    } else {
        t->parent = BB_SERIALIZE_JSON_TOK_ABSENT;
    }
    return idx;
}

// Parses a raw JSON number's text (not NUL-terminated, already
// grammar-validated by the scanner) into both int64_t and double
// representations, so get_i64()/get_f64() are both O(1) with no re-parse.
// strtoll()/strtod() are used rather than a hand-rolled parser: the
// scanner has already validated the grammar, so these only ever see
// well-formed input, and re-implementing correctly-rounded decimal-to-
// double conversion is not worth it here (unlike bb_serialize_json.c's
// WRITE-side formatter, which hand-rolls specifically to avoid
// snprintf/locale dependence in a hot render path -- this is a cold
// decode-time convenience, not a hot path).
static void tok_parse_num(bb_serialize_json_tok_t *t, const char *num, size_t num_len)
{
    char   buf[BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES + 1];
    size_t n = num_len < sizeof(buf) - 1 ? num_len : sizeof(buf) - 1;
    memcpy(buf, num, n);
    buf[n] = '\0';
    t->v.num.f64 = strtod(buf, NULL);
    t->v.num.i64 = strtoll(buf, NULL, 10);
}

// ---------------------------------------------------------------------------
// bb_serialize_json_ingest_t callbacks
// ---------------------------------------------------------------------------

static bb_err_t tok_begin_obj(void *ctx, const char *key, size_t key_len)
{
    bb_serialize_json_tok_recorder_t *rec = ctx;
    if (rec->depth >= BB_SERIALIZE_MAX_DEPTH) return BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE -- scanner enforces this bound first (same cap), unreachable in practice
    bb_serialize_json_tok_idx_t idx = tok_alloc(rec, BB_SERIALIZE_JSON_TOK_OBJ, key, key_len);
    if (idx < 0) return BB_ERR_NO_SPACE;
    rec->stack[rec->depth++] = idx;
    return BB_OK;
}

static bb_err_t tok_end_obj(void *ctx)
{
    bb_serialize_json_tok_recorder_t *rec = ctx;
    if (rec->depth == 0) return BB_ERR_INVALID_STATE;  // LCOV_EXCL_LINE -- scanner never calls end_obj unbalanced
    rec->depth--;
    return BB_OK;
}

static bb_err_t tok_begin_arr(void *ctx, const char *key, size_t key_len)
{
    bb_serialize_json_tok_recorder_t *rec = ctx;
    if (rec->depth >= BB_SERIALIZE_MAX_DEPTH) return BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE -- scanner enforces this bound first (same cap), unreachable in practice
    bb_serialize_json_tok_idx_t idx = tok_alloc(rec, BB_SERIALIZE_JSON_TOK_ARR, key, key_len);
    if (idx < 0) return BB_ERR_NO_SPACE;
    rec->stack[rec->depth++] = idx;
    return BB_OK;
}

static bb_err_t tok_end_arr(void *ctx)
{
    bb_serialize_json_tok_recorder_t *rec = ctx;
    if (rec->depth == 0) return BB_ERR_INVALID_STATE;  // LCOV_EXCL_LINE -- scanner never calls end_arr unbalanced
    rec->depth--;
    return BB_OK;
}

static bb_err_t tok_value_num(void *ctx, const char *key, size_t key_len,
                               const char *num, size_t num_len)
{
    bb_serialize_json_tok_recorder_t *rec = ctx;
    bb_serialize_json_tok_idx_t idx = tok_alloc(rec, BB_SERIALIZE_JSON_TOK_NUM, key, key_len);
    if (idx < 0) return BB_ERR_NO_SPACE;
    tok_parse_num(&rec->pool[idx], num, num_len);
    return BB_OK;
}

static bb_err_t tok_value_bool(void *ctx, const char *key, size_t key_len, bool v)
{
    bb_serialize_json_tok_recorder_t *rec = ctx;
    bb_serialize_json_tok_idx_t idx = tok_alloc(rec, BB_SERIALIZE_JSON_TOK_BOOL, key, key_len);
    if (idx < 0) return BB_ERR_NO_SPACE;
    rec->pool[idx].v.b = v;
    return BB_OK;
}

static bb_err_t tok_value_null(void *ctx, const char *key, size_t key_len)
{
    bb_serialize_json_tok_recorder_t *rec = ctx;
    bb_serialize_json_tok_idx_t idx = tok_alloc(rec, BB_SERIALIZE_JSON_TOK_NULL, key, key_len);
    if (idx < 0) return BB_ERR_NO_SPACE;
    return BB_OK;
}

static bb_err_t tok_value_str_chunk(void *ctx, const char *key, size_t key_len,
                                     const char *chunk, size_t chunk_len, bool is_final,
                                     bb_serialize_json_span_t span_provenance)
{
    bb_serialize_json_tok_recorder_t *rec = ctx;

    // Structural misuse guard: BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED
    // ONLY ever occurs when this sink is driven by the streaming
    // bb_serialize_json_scan_begin()/_feed()/_end() entry points -- this
    // recorder is bounded-mode ONLY (see the file banner in
    // bb_serialize_json.h). Reject rather than silently falling back to a
    // (technically safe, since it copies immediately) arena-copy path,
    // since accepting it would defeat the caller-visible contract.
    if (span_provenance == BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED) return BB_ERR_INVALID_STATE;

    if (!rec->str_open) {
        bb_serialize_json_tok_idx_t idx = tok_alloc(rec, BB_SERIALIZE_JSON_TOK_STR, key, key_len);
        if (idx < 0) return BB_ERR_NO_SPACE;
        rec->str_open = true;
        rec->str_tok_idx = idx;
        rec->str_use_arena = false;
        rec->str_arena_start = rec->arena_used;
        rec->str_len = 0;

        if (is_final && span_provenance == BB_SERIALIZE_JSON_SPAN_CALLER_STABLE) {
            // Zero-copy fast path: the ONLY call for this value (bounded-
            // mode's escape-free-string contract) -- record a direct span
            // into the caller's `buf`, no arena bytes spent.
            rec->pool[idx].v.str.ptr = chunk;
            rec->pool[idx].v.str.len = (uint32_t)chunk_len;
            rec->str_open = false;
            return BB_OK;
        }
        rec->str_use_arena = true;
    }

    // Assemble via arena: copy every chunk (direct-span runs and
    // decoded-escape scratch alike) contiguously, regardless of this
    // chunk's own provenance -- see the STRING VALUE STORAGE note in
    // bb_serialize_json.h. chunk_len is always > 0 here: this recorder
    // rejects BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED outright (see the
    // check at the top of this function), so it is structurally restricted
    // to bounded-mode calls only -- and per the BOUNDED-mode clause of the
    // value_str_chunk CONTRACT documented on bb_serialize_json_ingest_t in
    // bb_serialize_json.h, the ONLY zero-length call bounded mode ever makes
    // is the empty-string case (is_final on the very first call,
    // CALLER_STABLE), which the zero-copy fast path above already returns
    // from -- so every call that reaches this point already has
    // chunk_len > 0 by that contract.
    if (chunk_len > 0) {  // LCOV_EXCL_BR_LINE -- false arm structurally unreachable per the value_str_chunk contract, see above
        if (rec->arena == NULL || rec->arena_used + chunk_len > rec->arena_cap) return BB_ERR_NO_SPACE;
        memcpy(rec->arena + rec->arena_used, chunk, chunk_len);
        rec->arena_used += chunk_len;
        rec->str_len += chunk_len;
    }
    if (is_final) {
        rec->pool[rec->str_tok_idx].v.str.ptr = rec->arena + rec->str_arena_start;
        rec->pool[rec->str_tok_idx].v.str.len = (uint32_t)rec->str_len;
        rec->str_open = false;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_serialize_json_tok_recorder_init(bb_serialize_json_tok_recorder_t *rec,
                                              const char *buf, size_t len,
                                              bb_serialize_json_tok_t *pool, size_t pool_cap,
                                              char *arena, size_t arena_cap)
{
    if (rec == NULL || buf == NULL || pool == NULL || pool_cap == 0) return BB_ERR_INVALID_ARG;

    memset(rec, 0, sizeof(*rec));
    rec->buf = buf;
    rec->buf_len = len;
    rec->pool = pool;
    rec->pool_cap = pool_cap;
    rec->arena = arena;
    rec->arena_cap = arena_cap;
    return BB_OK;
}

bb_serialize_json_ingest_t bb_serialize_json_tok_recorder_ingest(bb_serialize_json_tok_recorder_t *rec)
{
    bb_serialize_json_ingest_t sink = {
        .ctx = rec,
        .begin_obj = tok_begin_obj,
        .end_obj = tok_end_obj,
        .begin_arr = tok_begin_arr,
        .end_arr = tok_end_arr,
        .value_num = tok_value_num,
        .value_bool = tok_value_bool,
        .value_null = tok_value_null,
        .value_str_chunk = tok_value_str_chunk,
    };
    return sink;
}

bb_serialize_json_tok_idx_t bb_serialize_json_tok_root(const bb_serialize_json_tok_recorder_t *rec)
{
    if (rec == NULL || rec->pool_n == 0) return BB_SERIALIZE_JSON_TOK_ABSENT;
    return 0;
}

bool bb_serialize_json_tok_is_obj(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx)
{
    return tok_valid(rec, idx) && rec->pool[idx].type == BB_SERIALIZE_JSON_TOK_OBJ;
}

bool bb_serialize_json_tok_is_arr(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx)
{
    return tok_valid(rec, idx) && rec->pool[idx].type == BB_SERIALIZE_JSON_TOK_ARR;
}

bool bb_serialize_json_tok_is_str(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx)
{
    return tok_valid(rec, idx) && rec->pool[idx].type == BB_SERIALIZE_JSON_TOK_STR;
}

bool bb_serialize_json_tok_is_num(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx)
{
    return tok_valid(rec, idx) && rec->pool[idx].type == BB_SERIALIZE_JSON_TOK_NUM;
}

bool bb_serialize_json_tok_is_bool(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx)
{
    return tok_valid(rec, idx) && rec->pool[idx].type == BB_SERIALIZE_JSON_TOK_BOOL;
}

bool bb_serialize_json_tok_is_null(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx)
{
    return tok_valid(rec, idx) && rec->pool[idx].type == BB_SERIALIZE_JSON_TOK_NULL;
}

bb_serialize_json_tok_idx_t bb_serialize_json_tok_obj_get(const bb_serialize_json_tok_recorder_t *rec,
                                                           bb_serialize_json_tok_idx_t obj,
                                                           const char *key, size_t key_len)
{
    if (!bb_serialize_json_tok_is_obj(rec, obj)) return BB_SERIALIZE_JSON_TOK_ABSENT;

    uint16_t remaining = rec->pool[obj].child_count;
    for (size_t i = (size_t)obj + 1; i < rec->pool_n && remaining > 0; i++) {
        if (rec->pool[i].parent != obj) continue;
        remaining--;
        if (rec->pool[i].key_len == key_len && memcmp(rec->pool[i].key, key, key_len) == 0) {
            return (bb_serialize_json_tok_idx_t)i;
        }
    }
    return BB_SERIALIZE_JSON_TOK_ABSENT;
}

int32_t bb_serialize_json_tok_arr_size(const bb_serialize_json_tok_recorder_t *rec,
                                        bb_serialize_json_tok_idx_t arr)
{
    if (!bb_serialize_json_tok_is_arr(rec, arr)) return -1;
    return (int32_t)rec->pool[arr].child_count;
}

bb_serialize_json_tok_idx_t bb_serialize_json_tok_arr_at(const bb_serialize_json_tok_recorder_t *rec,
                                                          bb_serialize_json_tok_idx_t arr, size_t elem_idx)
{
    if (!bb_serialize_json_tok_is_arr(rec, arr)) return BB_SERIALIZE_JSON_TOK_ABSENT;

    size_t seen = 0;
    for (size_t i = (size_t)arr + 1; i < rec->pool_n; i++) {
        if (rec->pool[i].parent != arr) continue;
        if (seen == elem_idx) return (bb_serialize_json_tok_idx_t)i;
        seen++;
    }
    return BB_SERIALIZE_JSON_TOK_ABSENT;
}

bool bb_serialize_json_tok_get_str(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                                    const char **out_ptr, size_t *out_len)
{
    if (!bb_serialize_json_tok_is_str(rec, idx)) return false;
    if (out_ptr != NULL) *out_ptr = rec->pool[idx].v.str.ptr;
    if (out_len != NULL) *out_len = rec->pool[idx].v.str.len;
    return true;
}

bool bb_serialize_json_tok_get_i64(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                                    int64_t *out)
{
    if (!bb_serialize_json_tok_is_num(rec, idx)) return false;
    if (out != NULL) *out = rec->pool[idx].v.num.i64;
    return true;
}

bool bb_serialize_json_tok_get_f64(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                                    double *out)
{
    if (!bb_serialize_json_tok_is_num(rec, idx)) return false;
    if (out != NULL) *out = rec->pool[idx].v.num.f64;
    return true;
}

bool bb_serialize_json_tok_get_bool(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                                     bool *out)
{
    if (!bb_serialize_json_tok_is_bool(rec, idx)) return false;
    if (out != NULL) *out = rec->pool[idx].v.b;
    return true;
}
