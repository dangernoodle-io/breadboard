#include "unity.h"
#include "bb_serialize_compose.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Recording mock emit -- appends one token per callback to a fixed, file-scope
// array (no heap). Doubles as a call-counting mock (s_rec_n) and a
// nesting-depth mock (s_depth / s_max_depth). Minimal op set: this file only
// needs I64 scalars plus begin_obj/end_obj bracketing.
// ---------------------------------------------------------------------------

typedef enum {
    OP_BEGIN_OBJ,
    OP_END_OBJ,
    OP_I64,
} rec_op_t;

typedef struct {
    rec_op_t    op;
    const char *key;
    int64_t     i64;
} rec_t;

#define REC_MAX 64

static rec_t  s_rec[REC_MAX];
static size_t s_rec_n;
static unsigned s_depth;
static unsigned s_max_depth;

static void rec_reset(void)
{
    s_rec_n = 0;
    s_depth = 0;
    s_max_depth = 0;
}

static rec_t *rec_push(rec_op_t op, const char *key)
{
    TEST_ASSERT_TRUE(s_rec_n < REC_MAX);
    rec_t *r = &s_rec[s_rec_n++];
    memset(r, 0, sizeof(*r));
    r->op = op;
    r->key = key;
    return r;
}

static void mock_begin_obj(void *ctx, const char *key)
{
    (void)ctx;
    rec_push(OP_BEGIN_OBJ, key);
    s_depth++;
    if (s_depth > s_max_depth) s_max_depth = s_depth;
}

static void mock_end_obj(void *ctx)
{
    (void)ctx;
    rec_push(OP_END_OBJ, NULL);
    s_depth--;
}

// Unused by any test in this file (compose never calls begin/end_arr, and
// none of the fixture descriptors below use BB_TYPE_ARR) -- present only
// because bb_serialize_emit_t requires every vtable slot to be non-NULL.
static void mock_begin_arr(void *ctx, const char *key) { (void)ctx; (void)key; }
static void mock_end_arr(void *ctx) { (void)ctx; }

static void mock_emit_i64(void *ctx, const char *key, int64_t v)
{
    (void)ctx;
    rec_push(OP_I64, key)->i64 = v;
}

static void mock_emit_u64(void *ctx, const char *key, uint64_t v) { (void)ctx; (void)key; (void)v; }
static void mock_emit_f64(void *ctx, const char *key, double v) { (void)ctx; (void)key; (void)v; }
static void mock_emit_bool(void *ctx, const char *key, bool v) { (void)ctx; (void)key; (void)v; }
static void mock_emit_str(void *ctx, const char *key, const char *s, size_t len)
{
    (void)ctx; (void)key; (void)s; (void)len;
}
static void mock_emit_null(void *ctx, const char *key) { (void)ctx; (void)key; }

static const bb_serialize_emit_t s_mock_emit = {
    .format_id = BB_FORMAT_NONE,
    .ctx = NULL,
    .begin_obj = mock_begin_obj,
    .end_obj = mock_end_obj,
    .begin_arr = mock_begin_arr,
    .end_arr = mock_end_arr,
    .emit_i64 = mock_emit_i64,
    .emit_u64 = mock_emit_u64,
    .emit_f64 = mock_emit_f64,
    .emit_bool = mock_emit_bool,
    .emit_str = mock_emit_str,
    .emit_null = mock_emit_null,
};

static void assert_key(const char *expected, const char *actual)
{
    if (!expected) {
        TEST_ASSERT_NULL(actual);
    } else {
        TEST_ASSERT_NOT_NULL(actual);
        TEST_ASSERT_EQUAL_STRING(expected, actual);
    }
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Flat: a single scalar field, no children -- the "leaf" desc used wherever
// a test wants zero inherent nesting of its own (so any begin_obj/end_obj
// observed is attributable to compose's OWN wrapper, never the inner walk).
typedef struct {
    int64_t v;
} flat_snap_t;

static const bb_serialize_field_t s_flat_fields[] = {
    { .key = "v", .type = BB_TYPE_I64, .offset = offsetof(flat_snap_t, v) },
};

static const bb_serialize_desc_t s_flat_desc = {
    .type_name = "flat_snap_t",
    .fields = s_flat_fields,
    .n_fields = 1,
    .snap_size = sizeof(flat_snap_t),
};

// Nested: one OBJ child -- gives bb_serialize_walk() its OWN inherent
// nesting level, distinct from compose's wrapper level, for the
// flat-nesting-proxy test.
typedef struct {
    int64_t x;
    int64_t y;
} point_t;

typedef struct {
    int64_t id;
    point_t pos;
} nested_snap_t;

static const bb_serialize_field_t s_point_fields[] = {
    { .key = "x", .type = BB_TYPE_I64, .offset = offsetof(point_t, x) },
    { .key = "y", .type = BB_TYPE_I64, .offset = offsetof(point_t, y) },
};

static const bb_serialize_field_t s_nested_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(nested_snap_t, id) },
    { .key = "pos", .type = BB_TYPE_OBJ, .offset = offsetof(nested_snap_t, pos),
      .children = s_point_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_nested_desc = {
    .type_name = "nested_snap_t",
    .fields = s_nested_fields,
    .n_fields = 2,
    .snap_size = sizeof(nested_snap_t),
};

// ---------------------------------------------------------------------------
// 1. OBJECT shape golden string -- drive the real JSON backend, hand-written
// golden compare (not a re-render).
// ---------------------------------------------------------------------------

void test_bb_serialize_compose_object_shape_json_golden(void)
{
    flat_snap_t snap_a = { .v = 1 };
    flat_snap_t snap_b = { .v = 2 };
    nested_snap_t snap_c = { .id = 9, .pos = { .x = 3, .y = 4 } };

    const bb_serialize_compose_entry_t entries[] = {
        { .name = "a", .desc = &s_flat_desc, .snap = &snap_a },
        { .name = "b", .desc = &s_flat_desc, .snap = &snap_b },
        { .name = "c", .desc = &s_nested_desc, .snap = &snap_c },
    };

    // compose_walk does not emit an outer root container -- same convention
    // as bb_serialize_walk() -- so the caller (here, the test) owns root
    // framing, matching bb_serialize_json_render()'s own convention: it
    // writes the root braces directly (not via emit->begin_obj/end_obj,
    // which -- like bb_serialize_walk() -- assumes it's already INSIDE a
    // container, not framing the root itself).
    char buf[256];
    buf[0] = '{';
    bb_serialize_json_ctx_t jctx;
    bb_serialize_json_ctx_init(&jctx, buf + 1, sizeof(buf) - 2);
    bb_serialize_emit_t emit = bb_serialize_json_emit(&jctx);

    bb_err_t rc = bb_serialize_compose_walk(entries, 3, BB_SERIALIZE_COMPOSE_OBJECT, &emit);
    TEST_ASSERT_EQUAL(BB_OK, jctx.err);
    buf[1 + jctx.len] = '}';
    buf[1 + jctx.len + 1] = '\0';

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING(
        "{\"a\":{\"v\":1},\"b\":{\"v\":2},\"c\":{\"id\":9,\"pos\":{\"x\":3,\"y\":4}}}", buf);
}

// ---------------------------------------------------------------------------
// 2. Flat-nesting proxy: peak container depth doesn't grow with n.
// ---------------------------------------------------------------------------

void test_bb_serialize_compose_peak_depth_independent_of_entry_count(void)
{
    nested_snap_t snap = { .id = 1, .pos = { .x = 1, .y = 2 } };
    const bb_serialize_compose_entry_t one[] = {
        { .name = "e0", .desc = &s_nested_desc, .snap = &snap },
    };
    const bb_serialize_compose_entry_t five[] = {
        { .name = "e0", .desc = &s_nested_desc, .snap = &snap },
        { .name = "e1", .desc = &s_nested_desc, .snap = &snap },
        { .name = "e2", .desc = &s_nested_desc, .snap = &snap },
        { .name = "e3", .desc = &s_nested_desc, .snap = &snap },
        { .name = "e4", .desc = &s_nested_desc, .snap = &snap },
    };

    rec_reset();
    bb_err_t rc1 = bb_serialize_compose_walk(one, 1, BB_SERIALIZE_COMPOSE_OBJECT, &s_mock_emit);
    unsigned max1 = s_max_depth;

    rec_reset();
    bb_err_t rc5 = bb_serialize_compose_walk(five, 5, BB_SERIALIZE_COMPOSE_OBJECT, &s_mock_emit);
    unsigned max5 = s_max_depth;

    TEST_ASSERT_EQUAL(BB_OK, rc1);
    TEST_ASSERT_EQUAL(BB_OK, rc5);
    // compose's own wrapper (depth 1) + nested_desc's inner "pos" OBJ
    // (depth 2) -- identical peak regardless of entry count, because at
    // most one entry's wrapper+walk is ever open at once (a loop, not
    // nested recursion).
    TEST_ASSERT_EQUAL_UINT(2, max1);
    TEST_ASSERT_EQUAL_UINT(max1, max5);
    // 5 entries * (begin_obj,id,begin_obj,x,y,end_obj,end_obj) = 5*7 ops.
    TEST_ASSERT_EQUAL_UINT(5 * 7, s_rec_n);
}

// ---------------------------------------------------------------------------
// 3. n == 0 -> BB_OK, zero emit calls.
// ---------------------------------------------------------------------------

void test_bb_serialize_compose_empty_returns_ok_zero_calls(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_compose_walk(NULL, 0, BB_SERIALIZE_COMPOSE_OBJECT, &s_mock_emit);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(0, s_rec_n);
}

// ---------------------------------------------------------------------------
// 4. Single-entry degenerate per shape.
// ---------------------------------------------------------------------------

void test_bb_serialize_compose_single_entry_object_shape(void)
{
    flat_snap_t snap = { .v = 7 };
    const bb_serialize_compose_entry_t entries[] = {
        { .name = "only", .desc = &s_flat_desc, .snap = &snap },
    };

    rec_reset();
    bb_err_t rc = bb_serialize_compose_walk(entries, 1, BB_SERIALIZE_COMPOSE_OBJECT, &s_mock_emit);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(3, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[0].op);
    assert_key("only", s_rec[0].key);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[1].op);
    assert_key("v", s_rec[1].key);
    TEST_ASSERT_EQUAL_INT64(7, s_rec[1].i64);
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[2].op);
}

void test_bb_serialize_compose_single_entry_array_shape(void)
{
    flat_snap_t snap = { .v = 7 };
    const bb_serialize_compose_entry_t entries[] = {
        { .name = "ignored", .desc = &s_flat_desc, .snap = &snap },
    };

    rec_reset();
    bb_err_t rc = bb_serialize_compose_walk(entries, 1, BB_SERIALIZE_COMPOSE_ARRAY, &s_mock_emit);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(3, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[0].op);
    assert_key(NULL, s_rec[0].key);  // ARRAY shape is unkeyed -- name is ignored
    TEST_ASSERT_EQUAL(OP_I64, s_rec[1].op);
    TEST_ASSERT_EQUAL_INT64(7, s_rec[1].i64);
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[2].op);
}

void test_bb_serialize_compose_single_entry_raw_shape_matches_direct_walk(void)
{
    flat_snap_t snap = { .v = 7 };
    const bb_serialize_compose_entry_t entries[] = {
        { .name = "ignored", .desc = &s_flat_desc, .snap = &snap },
    };

    rec_reset();
    bb_err_t rc = bb_serialize_compose_walk(entries, 1, BB_SERIALIZE_COMPOSE_RAW, &s_mock_emit);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    rec_t raw_via_compose = s_rec[0];

    rec_reset();
    bb_serialize_walk(&s_flat_desc, &snap, &s_mock_emit);
    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);

    TEST_ASSERT_EQUAL(raw_via_compose.op, s_rec[0].op);
    assert_key(raw_via_compose.key, s_rec[0].key);
    TEST_ASSERT_EQUAL_INT64(raw_via_compose.i64, s_rec[0].i64);
}

// ---------------------------------------------------------------------------
// 5. RAW shape: entries merge flatly -- no begin_obj/end_obj attributable to
// compose itself (both entries use the flat, childless desc, so ANY
// begin_obj/end_obj recorded could only come from compose's own wrapper).
// ---------------------------------------------------------------------------

void test_bb_serialize_compose_raw_shape_no_wrapper_calls(void)
{
    flat_snap_t snap_a = { .v = 1 };
    flat_snap_t snap_b = { .v = 2 };
    const bb_serialize_compose_entry_t entries[] = {
        { .name = "a", .desc = &s_flat_desc, .snap = &snap_a },
        { .name = "b", .desc = &s_flat_desc, .snap = &snap_b },
    };

    rec_reset();
    bb_err_t rc = bb_serialize_compose_walk(entries, 2, BB_SERIALIZE_COMPOSE_RAW, &s_mock_emit);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    TEST_ASSERT_EQUAL_INT64(1, s_rec[0].i64);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[1].op);
    TEST_ASSERT_EQUAL_INT64(2, s_rec[1].i64);
}

// ---------------------------------------------------------------------------
// Defensive branches
// ---------------------------------------------------------------------------

void test_bb_serialize_compose_null_emit_returns_invalid_arg(void)
{
    flat_snap_t snap = { .v = 1 };
    const bb_serialize_compose_entry_t entries[] = {
        { .desc = &s_flat_desc, .snap = &snap },
    };
    bb_err_t rc = bb_serialize_compose_walk(entries, 1, BB_SERIALIZE_COMPOSE_RAW, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_bb_serialize_compose_null_entries_with_nonzero_n_returns_invalid_arg(void)
{
    bb_err_t rc = bb_serialize_compose_walk(NULL, 1, BB_SERIALIZE_COMPOSE_RAW, &s_mock_emit);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_bb_serialize_compose_entry_null_desc_returns_invalid_arg(void)
{
    flat_snap_t snap = { .v = 1 };
    const bb_serialize_compose_entry_t entries[] = {
        { .name = "a", .desc = &s_flat_desc, .snap = &snap },
        { .name = "b", .desc = NULL, .snap = NULL },
    };

    rec_reset();
    bb_err_t rc = bb_serialize_compose_walk(entries, 2, BB_SERIALIZE_COMPOSE_RAW, &s_mock_emit);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
    // Entry 0 was already fully walked before entry 1's NULL desc was
    // detected -- abort is immediate, but not retroactive.
    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL_INT64(1, s_rec[0].i64);
}

static bb_err_t gather_ok(void *snap, void *ctx)
{
    (void)ctx;
    ((flat_snap_t *)snap)->v = 42;
    return BB_OK;
}

static bb_err_t gather_fails(void *snap, void *ctx)
{
    (void)snap;
    (void)ctx;
    return BB_ERR_VALIDATION;
}

void test_bb_serialize_compose_gather_failure_aborts_whole_walk(void)
{
    flat_snap_t snap_a = { .v = 0 };
    flat_snap_t snap_b = { .v = 0 };
    flat_snap_t snap_c = { .v = 0 };
    const bb_serialize_compose_entry_t entries[] = {
        { .name = "a", .desc = &s_flat_desc, .snap = &snap_a, .gather = gather_ok },
        { .name = "b", .desc = &s_flat_desc, .snap = &snap_b, .gather = gather_fails },
        { .name = "c", .desc = &s_flat_desc, .snap = &snap_c, .gather = gather_ok },
    };

    rec_reset();
    bb_err_t rc = bb_serialize_compose_walk(entries, 3, BB_SERIALIZE_COMPOSE_OBJECT, &s_mock_emit);

    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    // Entry "a" gathered + fully walked (begin_obj, v, end_obj = 3 ops).
    // Entry "b"'s gather ran and failed BEFORE any emit call for "b" was
    // made -- zero ops attributable to it. Entry "c" was never reached at
    // all -- its gather never ran (snap_c.v is still 0, not 42).
    TEST_ASSERT_EQUAL_UINT(3, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[0].op);
    assert_key("a", s_rec[0].key);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[1].op);
    TEST_ASSERT_EQUAL_INT64(42, s_rec[1].i64);
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[2].op);
    TEST_ASSERT_EQUAL_INT64(0, snap_c.v);
}
