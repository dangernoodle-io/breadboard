#include "unity.h"
#include "bb_serialize.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Recording emit mock -- appends a token per callback to a fixed, file-scope
// array (no heap). Call order is fidelity-load-bearing, so tests assert the
// full recorded sequence, not just membership.
// ---------------------------------------------------------------------------

typedef enum {
    OP_BEGIN_OBJ,
    OP_END_OBJ,
    OP_BEGIN_ARR,
    OP_END_ARR,
    OP_I64,
    OP_U64,
    OP_F64,
    OP_BOOL,
    OP_STR,
    OP_NUL,
} rec_op_t;

typedef struct {
    rec_op_t    op;
    const char *key;
    union {
        int64_t     i64;
        uint64_t    u64;
        double      f64;
        bool        b;
    } num;
    char   str_val[64];
    size_t str_len;
} rec_t;

#define REC_MAX 64

static rec_t  s_rec[REC_MAX];
static size_t s_rec_n;

static void rec_reset(void) { s_rec_n = 0; }

static rec_t *rec_push(rec_op_t op, const char *key)
{
    TEST_ASSERT_TRUE(s_rec_n < REC_MAX);
    rec_t *r = &s_rec[s_rec_n++];
    memset(r, 0, sizeof(*r));
    r->op = op;
    r->key = key;
    return r;
}

static void mock_begin_obj(void *ctx, const char *key) { (void)ctx; rec_push(OP_BEGIN_OBJ, key); }
static void mock_end_obj(void *ctx) { (void)ctx; rec_push(OP_END_OBJ, NULL); }
static void mock_begin_arr(void *ctx, const char *key) { (void)ctx; rec_push(OP_BEGIN_ARR, key); }
static void mock_end_arr(void *ctx) { (void)ctx; rec_push(OP_END_ARR, NULL); }

static void mock_emit_i64(void *ctx, const char *key, int64_t v)
{
    (void)ctx;
    rec_push(OP_I64, key)->num.i64 = v;
}

static void mock_emit_u64(void *ctx, const char *key, uint64_t v)
{
    (void)ctx;
    rec_push(OP_U64, key)->num.u64 = v;
}

static void mock_emit_f64(void *ctx, const char *key, double v)
{
    (void)ctx;
    rec_push(OP_F64, key)->num.f64 = v;
}

static void mock_emit_bool(void *ctx, const char *key, bool v)
{
    (void)ctx;
    rec_push(OP_BOOL, key)->num.b = v;
}

static void mock_emit_str(void *ctx, const char *key, const char *s, size_t len)
{
    (void)ctx;
    rec_t *r = rec_push(OP_STR, key);
    size_t n = len < sizeof(r->str_val) - 1 ? len : sizeof(r->str_val) - 1;
    if (s && n) memcpy(r->str_val, s, n);
    r->str_val[n] = '\0';
    r->str_len = len;
}

static void mock_emit_null(void *ctx, const char *key) { (void)ctx; rec_push(OP_NUL, key); }

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
// 1. flat scalars in table order
// ---------------------------------------------------------------------------

typedef struct {
    int64_t  i;
    uint64_t u;
    double   f;
    bool     b;
} flat_snap_t;

static const bb_serialize_field_t s_flat_fields[] = {
    { .key = "i", .type = BB_TYPE_I64, .offset = offsetof(flat_snap_t, i) },
    { .key = "u", .type = BB_TYPE_U64, .offset = offsetof(flat_snap_t, u) },
    { .key = "f", .type = BB_TYPE_F64, .offset = offsetof(flat_snap_t, f) },
    { .key = "b", .type = BB_TYPE_BOOL, .offset = offsetof(flat_snap_t, b) },
};

static const bb_serialize_desc_t s_flat_desc = {
    .type_name = "flat_snap_t",
    .fields = s_flat_fields,
    .n_fields = 4,
    .snap_size = sizeof(flat_snap_t),
};

void test_bb_serialize_flat_scalars(void)
{
    rec_reset();
    flat_snap_t snap = { .i = -7, .u = 42, .f = 3.5, .b = true };

    bb_serialize_walk(&s_flat_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(4, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("i", s_rec[0].key);
    TEST_ASSERT_EQUAL_INT64(-7, s_rec[0].num.i64);
    TEST_ASSERT_EQUAL(OP_U64, s_rec[1].op);
    assert_key("u", s_rec[1].key);
    TEST_ASSERT_EQUAL_UINT64(42, s_rec[1].num.u64);
    TEST_ASSERT_EQUAL(OP_F64, s_rec[2].op);
    assert_key("f", s_rec[2].key);
    TEST_ASSERT_EQUAL_DOUBLE(3.5, s_rec[2].num.f64);
    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[3].op);
    assert_key("b", s_rec[3].key);
    TEST_ASSERT_TRUE(s_rec[3].num.b);
}

// ---------------------------------------------------------------------------
// 2. nested OBJ bracketing + correct child offsets
// ---------------------------------------------------------------------------

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

void test_bb_serialize_nested_obj(void)
{
    rec_reset();
    nested_snap_t snap = { .id = 1, .pos = { .x = 10, .y = 20 } };

    bb_serialize_walk(&s_nested_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(5, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("id", s_rec[0].key);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[1].op);
    assert_key("pos", s_rec[1].key);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[2].op);
    assert_key("x", s_rec[2].key);
    TEST_ASSERT_EQUAL_INT64(10, s_rec[2].num.i64);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[3].op);
    assert_key("y", s_rec[3].key);
    TEST_ASSERT_EQUAL_INT64(20, s_rec[3].num.i64);
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[4].op);
}

// ---------------------------------------------------------------------------
// 3. present-gate omit -> zero emit for that field
// ---------------------------------------------------------------------------

typedef struct {
    int64_t a;
    int64_t b;
    bool    b_present;
} gated_snap_t;

static bool gate_b_present(const void *snap)
{
    return ((const gated_snap_t *)snap)->b_present;
}

static const bb_serialize_field_t s_gated_fields[] = {
    { .key = "a", .type = BB_TYPE_I64, .offset = offsetof(gated_snap_t, a) },
    { .key = "b", .type = BB_TYPE_I64, .offset = offsetof(gated_snap_t, b), .present = gate_b_present },
};

static const bb_serialize_desc_t s_gated_desc = {
    .type_name = "gated_snap_t",
    .fields = s_gated_fields,
    .n_fields = 2,
    .snap_size = sizeof(gated_snap_t),
};

void test_bb_serialize_present_gate_omits_field(void)
{
    rec_reset();
    gated_snap_t snap = { .a = 1, .b = 2, .b_present = false };

    bb_serialize_walk(&s_gated_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("a", s_rec[0].key);
}

void test_bb_serialize_present_gate_emits_when_true(void)
{
    rec_reset();
    gated_snap_t snap = { .a = 1, .b = 2, .b_present = true };

    bb_serialize_walk(&s_gated_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    assert_key("b", s_rec[1].key);
    TEST_ASSERT_EQUAL_INT64(2, s_rec[1].num.i64);
}

// ---------------------------------------------------------------------------
// 4. STR bound: non-NUL-terminated char[N] yields exactly strnlen bytes
// ---------------------------------------------------------------------------

typedef struct {
    char status[4];
} str_snap_t;

static const bb_serialize_field_t s_str_fields[] = {
    { .key = "status", .type = BB_TYPE_STR, .offset = offsetof(str_snap_t, status), .max_len = 4 },
};

static const bb_serialize_desc_t s_str_desc = {
    .type_name = "str_snap_t",
    .fields = s_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(str_snap_t),
};

void test_bb_serialize_str_bound_no_nul_terminator(void)
{
    rec_reset();
    str_snap_t snap;
    memcpy(snap.status, "abcd", 4);  // NOT NUL-terminated within the 4-byte bound

    bb_serialize_walk(&s_str_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_STR, s_rec[0].op);
    TEST_ASSERT_EQUAL_UINT(4, s_rec[0].str_len);
    TEST_ASSERT_EQUAL_STRING_LEN("abcd", s_rec[0].str_val, 4);
}

void test_bb_serialize_str_bound_nul_terminated_short(void)
{
    rec_reset();
    str_snap_t snap;
    memcpy(snap.status, "ok\0\0", 4);

    bb_serialize_walk(&s_str_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL_UINT(2, s_rec[0].str_len);
    TEST_ASSERT_EQUAL_STRING("ok", s_rec[0].str_val);
}

// ---------------------------------------------------------------------------
// 5. STR_N: len-0 non-NULL -> emit_str len 0; NULL ptr -> emit_null
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_str_n_t sn;
} strn_snap_t;

static const bb_serialize_field_t s_strn_fields[] = {
    { .key = "sn", .type = BB_TYPE_STR_N, .offset = offsetof(strn_snap_t, sn) },
};

static const bb_serialize_desc_t s_strn_desc = {
    .type_name = "strn_snap_t",
    .fields = s_strn_fields,
    .n_fields = 1,
    .snap_size = sizeof(strn_snap_t),
};

void test_bb_serialize_str_n_empty_non_null_emits_empty_string(void)
{
    rec_reset();
    strn_snap_t snap = { .sn = { .ptr = "", .len = 0 } };

    bb_serialize_walk(&s_strn_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_STR, s_rec[0].op);
    TEST_ASSERT_EQUAL_UINT(0, s_rec[0].str_len);
}

void test_bb_serialize_str_n_null_ptr_emits_null(void)
{
    rec_reset();
    strn_snap_t snap = { .sn = { .ptr = NULL, .len = 5 } };

    bb_serialize_walk(&s_strn_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_NUL, s_rec[0].op);
    assert_key("sn", s_rec[0].key);
}

// ---------------------------------------------------------------------------
// 6. ARR of strings
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_arr_t tags;
} arr_str_snap_t;

static const bb_serialize_field_t s_arr_str_fields[] = {
    { .key = "tags", .type = BB_TYPE_ARR, .offset = offsetof(arr_str_snap_t, tags), .elem_type = BB_TYPE_STR,
      .max_len = 16 },
};

static const bb_serialize_desc_t s_arr_str_desc = {
    .type_name = "arr_str_snap_t",
    .fields = s_arr_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_str_snap_t),
};

void test_bb_serialize_arr_of_strings(void)
{
    rec_reset();
    const char *items[] = { "one", NULL, "three" };
    arr_str_snap_t snap = { .tags = { .items = items, .count = 3 } };

    bb_serialize_walk(&s_arr_str_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(5, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    assert_key("tags", s_rec[0].key);
    TEST_ASSERT_EQUAL(OP_STR, s_rec[1].op);
    assert_key(NULL, s_rec[1].key);
    TEST_ASSERT_EQUAL_STRING("one", s_rec[1].str_val);
    TEST_ASSERT_EQUAL(OP_NUL, s_rec[2].op);
    assert_key(NULL, s_rec[2].key);
    TEST_ASSERT_EQUAL(OP_STR, s_rec[3].op);
    TEST_ASSERT_EQUAL_STRING("three", s_rec[3].str_val);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[4].op);
}

void test_bb_serialize_arr_of_strings_empty(void)
{
    rec_reset();
    arr_str_snap_t snap = { .tags = { .items = NULL, .count = 0 } };

    bb_serialize_walk(&s_arr_str_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[1].op);
}

// ---------------------------------------------------------------------------
// 6b. ARR of scalars (I64/U64/F64/BOOL)
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_arr_t nums;
} arr_i64_snap_t;

static const bb_serialize_field_t s_arr_i64_fields[] = {
    { .key = "nums", .type = BB_TYPE_ARR, .offset = offsetof(arr_i64_snap_t, nums),
      .elem_type = BB_TYPE_I64 },
};

static const bb_serialize_desc_t s_arr_i64_desc = {
    .type_name = "arr_i64_snap_t",
    .fields = s_arr_i64_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_i64_snap_t),
};

void test_bb_serialize_arr_of_i64(void)
{
    rec_reset();
    const int64_t vals[] = { 1, 2, 3 };
    arr_i64_snap_t snap = { .nums = { .items = vals, .count = 3 } };

    bb_serialize_walk(&s_arr_i64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(5, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    assert_key("nums", s_rec[0].key);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[1].op);
    assert_key(NULL, s_rec[1].key);
    TEST_ASSERT_EQUAL_INT64(1, s_rec[1].num.i64);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[2].op);
    TEST_ASSERT_EQUAL_INT64(2, s_rec[2].num.i64);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[3].op);
    TEST_ASSERT_EQUAL_INT64(3, s_rec[3].num.i64);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[4].op);
}

void test_bb_serialize_arr_of_i64_empty(void)
{
    rec_reset();
    arr_i64_snap_t snap = { .nums = { .items = NULL, .count = 0 } };

    bb_serialize_walk(&s_arr_i64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[1].op);
}

void test_bb_serialize_arr_of_i64_single(void)
{
    rec_reset();
    const int64_t vals[] = { 42 };
    arr_i64_snap_t snap = { .nums = { .items = vals, .count = 1 } };

    bb_serialize_walk(&s_arr_i64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(3, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[1].op);
    TEST_ASSERT_EQUAL_INT64(42, s_rec[1].num.i64);
}

typedef struct {
    bb_serialize_arr_t nums;
} arr_u64_snap_t;

static const bb_serialize_field_t s_arr_u64_fields[] = {
    { .key = "nums", .type = BB_TYPE_ARR, .offset = offsetof(arr_u64_snap_t, nums),
      .elem_type = BB_TYPE_U64 },
};

static const bb_serialize_desc_t s_arr_u64_desc = {
    .type_name = "arr_u64_snap_t",
    .fields = s_arr_u64_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_u64_snap_t),
};

void test_bb_serialize_arr_of_u64(void)
{
    rec_reset();
    const uint64_t vals[] = { 10, 20 };
    arr_u64_snap_t snap = { .nums = { .items = vals, .count = 2 } };

    bb_serialize_walk(&s_arr_u64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(4, s_rec_n);
    TEST_ASSERT_EQUAL(OP_U64, s_rec[1].op);
    TEST_ASSERT_EQUAL_UINT64(10, s_rec[1].num.u64);
    TEST_ASSERT_EQUAL(OP_U64, s_rec[2].op);
    TEST_ASSERT_EQUAL_UINT64(20, s_rec[2].num.u64);
}

void test_bb_serialize_arr_of_u64_empty(void)
{
    rec_reset();
    arr_u64_snap_t snap = { .nums = { .items = NULL, .count = 0 } };

    bb_serialize_walk(&s_arr_u64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[1].op);
}

void test_bb_serialize_arr_of_u64_single(void)
{
    rec_reset();
    const uint64_t vals[] = { 42 };
    arr_u64_snap_t snap = { .nums = { .items = vals, .count = 1 } };

    bb_serialize_walk(&s_arr_u64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(3, s_rec_n);
    TEST_ASSERT_EQUAL(OP_U64, s_rec[1].op);
    TEST_ASSERT_EQUAL_UINT64(42, s_rec[1].num.u64);
}

typedef struct {
    bb_serialize_arr_t nums;
} arr_f64_snap_t;

static const bb_serialize_field_t s_arr_f64_fields[] = {
    { .key = "nums", .type = BB_TYPE_ARR, .offset = offsetof(arr_f64_snap_t, nums),
      .elem_type = BB_TYPE_F64 },
};

static const bb_serialize_desc_t s_arr_f64_desc = {
    .type_name = "arr_f64_snap_t",
    .fields = s_arr_f64_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_f64_snap_t),
};

void test_bb_serialize_arr_of_f64(void)
{
    rec_reset();
    const double vals[] = { 1.5, -2.25 };
    arr_f64_snap_t snap = { .nums = { .items = vals, .count = 2 } };

    bb_serialize_walk(&s_arr_f64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(4, s_rec_n);
    TEST_ASSERT_EQUAL(OP_F64, s_rec[1].op);
    TEST_ASSERT_EQUAL_DOUBLE(1.5, s_rec[1].num.f64);
    TEST_ASSERT_EQUAL(OP_F64, s_rec[2].op);
    TEST_ASSERT_EQUAL_DOUBLE(-2.25, s_rec[2].num.f64);
}

void test_bb_serialize_arr_of_f64_empty(void)
{
    rec_reset();
    arr_f64_snap_t snap = { .nums = { .items = NULL, .count = 0 } };

    bb_serialize_walk(&s_arr_f64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[1].op);
}

void test_bb_serialize_arr_of_f64_single(void)
{
    rec_reset();
    const double vals[] = { 1.5 };
    arr_f64_snap_t snap = { .nums = { .items = vals, .count = 1 } };

    bb_serialize_walk(&s_arr_f64_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(3, s_rec_n);
    TEST_ASSERT_EQUAL(OP_F64, s_rec[1].op);
    TEST_ASSERT_EQUAL_DOUBLE(1.5, s_rec[1].num.f64);
}

typedef struct {
    bb_serialize_arr_t flags;
} arr_bool_snap_t;

static const bb_serialize_field_t s_arr_bool_fields[] = {
    { .key = "flags", .type = BB_TYPE_ARR, .offset = offsetof(arr_bool_snap_t, flags),
      .elem_type = BB_TYPE_BOOL },
};

static const bb_serialize_desc_t s_arr_bool_desc = {
    .type_name = "arr_bool_snap_t",
    .fields = s_arr_bool_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_bool_snap_t),
};

void test_bb_serialize_arr_of_bool(void)
{
    rec_reset();
    const bool vals[] = { true, false };
    arr_bool_snap_t snap = { .flags = { .items = vals, .count = 2 } };

    bb_serialize_walk(&s_arr_bool_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(4, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[1].op);
    TEST_ASSERT_TRUE(s_rec[1].num.b);
    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[2].op);
    TEST_ASSERT_FALSE(s_rec[2].num.b);
}

void test_bb_serialize_arr_of_bool_empty(void)
{
    rec_reset();
    arr_bool_snap_t snap = { .flags = { .items = NULL, .count = 0 } };

    bb_serialize_walk(&s_arr_bool_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[1].op);
}

void test_bb_serialize_arr_of_bool_single(void)
{
    rec_reset();
    const bool vals[] = { true };
    arr_bool_snap_t snap = { .flags = { .items = vals, .count = 1 } };

    bb_serialize_walk(&s_arr_bool_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(3, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[1].op);
    TEST_ASSERT_TRUE(s_rec[1].num.b);
}

// ---------------------------------------------------------------------------
// 7. ARR of OBJ
// ---------------------------------------------------------------------------

typedef struct {
    int64_t id;
    int64_t val;
} elem_t;

typedef struct {
    bb_serialize_arr_t items;
} arr_obj_snap_t;

static const bb_serialize_field_t s_elem_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(elem_t, id) },
    { .key = "val", .type = BB_TYPE_I64, .offset = offsetof(elem_t, val) },
};

static const bb_serialize_field_t s_arr_obj_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(arr_obj_snap_t, items),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(elem_t),
      .children = s_elem_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_arr_obj_desc = {
    .type_name = "arr_obj_snap_t",
    .fields = s_arr_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_obj_snap_t),
};

void test_bb_serialize_arr_of_obj_two_elements(void)
{
    rec_reset();
    elem_t elems[2] = { { .id = 1, .val = 100 }, { .id = 2, .val = 200 } };
    arr_obj_snap_t snap = { .items = { .items = elems, .count = 2 } };

    bb_serialize_walk(&s_arr_obj_desc, &snap, &s_mock_emit);

    // begin_arr, {begin_obj,id,val,end_obj} x2, end_arr = 1 + 4*2 + 1
    TEST_ASSERT_EQUAL_UINT(10, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[1].op);
    assert_key(NULL, s_rec[1].key);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[2].op);
    TEST_ASSERT_EQUAL_INT64(1, s_rec[2].num.i64);
    TEST_ASSERT_EQUAL_INT64(100, s_rec[3].num.i64);
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[4].op);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[5].op);
    TEST_ASSERT_EQUAL_INT64(2, s_rec[6].num.i64);
    TEST_ASSERT_EQUAL_INT64(200, s_rec[7].num.i64);
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[8].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[9].op);
}

void test_bb_serialize_arr_of_obj_empty(void)
{
    rec_reset();
    arr_obj_snap_t snap = { .items = { .items = NULL, .count = 0 } };

    bb_serialize_walk(&s_arr_obj_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[1].op);
}

void test_bb_serialize_arr_of_obj_null_items_guarded(void)
{
    rec_reset();
    // NULL items with a nonzero count is caller UB -- must degrade to an
    // empty array (no deref), never crash.
    arr_obj_snap_t snap = { .items = { .items = NULL, .count = 3 } };

    bb_serialize_walk(&s_arr_obj_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[1].op);
}

// ---------------------------------------------------------------------------
// 8. depth guard: self-referential children bails at BB_SERIALIZE_MAX_DEPTH
// ---------------------------------------------------------------------------

typedef struct {
    int64_t marker;
} deep_snap_t;

static const bb_serialize_field_t s_deep_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_snap_t, marker) },
    // Self-referential OBJ child: `deep` recurses into itself forever
    // unless the walker's depth guard bails.
    { .key = "deep", .type = BB_TYPE_OBJ, .offset = 0,
      .children = s_deep_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_desc = {
    .type_name = "deep_snap_t",
    .fields = s_deep_fields,
    .n_fields = 2,
    .snap_size = sizeof(deep_snap_t),
};

void test_bb_serialize_depth_guard_bails_on_self_reference(void)
{
    rec_reset();
    deep_snap_t snap = { .marker = 1 };

    bb_serialize_walk(&s_deep_desc, &snap, &s_mock_emit);

    // Depths 0..MAX_DEPTH-1 each emit {marker, begin_obj("deep")} (2 records)
    // and recurse; at depth == MAX_DEPTH the guard bails, emitting only the
    // final marker (no begin_obj, no further recursion); unwinding then
    // emits one end_obj per recursed level. No overflow, no hang.
    TEST_ASSERT_EQUAL_UINT(BB_SERIALIZE_MAX_DEPTH * 2 + 1 + BB_SERIALIZE_MAX_DEPTH, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[1].op);
}

// ---------------------------------------------------------------------------
// 9. empty OBJ: begin_obj immediately followed by end_obj, no inner emits
// ---------------------------------------------------------------------------

typedef struct {
    int64_t marker;
} empty_child_t;

typedef struct {
    empty_child_t child;
} empty_obj_snap_t;

static const bb_serialize_field_t s_empty_obj_fields[] = {
    { .key = "child", .type = BB_TYPE_OBJ, .offset = offsetof(empty_obj_snap_t, child),
      .children = NULL, .n_children = 0 },
};

static const bb_serialize_desc_t s_empty_obj_desc = {
    .type_name = "empty_obj_snap_t",
    .fields = s_empty_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(empty_obj_snap_t),
};

void test_bb_serialize_empty_obj(void)
{
    rec_reset();
    empty_obj_snap_t snap = { .child = { .marker = 99 } };

    bb_serialize_walk(&s_empty_obj_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[0].op);
    assert_key("child", s_rec[0].key);
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[1].op);
}

// ---------------------------------------------------------------------------
// 10. present-gated OBJ / ARR: zero emits, no orphan begin_obj/begin_arr
// ---------------------------------------------------------------------------

typedef struct {
    point_t              pos;
    bb_serialize_arr_t   tags;
    bool                 container_present;
} gated_container_snap_t;

static bool gate_container_present(const void *snap)
{
    return ((const gated_container_snap_t *)snap)->container_present;
}

static const bb_serialize_field_t s_gated_container_fields[] = {
    { .key = "pos", .type = BB_TYPE_OBJ, .offset = offsetof(gated_container_snap_t, pos),
      .present = gate_container_present, .children = s_point_fields, .n_children = 2 },
    { .key = "tags", .type = BB_TYPE_ARR, .offset = offsetof(gated_container_snap_t, tags),
      .present = gate_container_present, .elem_type = BB_TYPE_STR, .max_len = 16 },
};

static const bb_serialize_desc_t s_gated_container_desc = {
    .type_name = "gated_container_snap_t",
    .fields = s_gated_container_fields,
    .n_fields = 2,
    .snap_size = sizeof(gated_container_snap_t),
};

void test_bb_serialize_present_gated_container(void)
{
    rec_reset();
    const char *items[] = { "one" };
    gated_container_snap_t snap = {
        .pos = { .x = 1, .y = 2 },
        .tags = { .items = items, .count = 1 },
        .container_present = false,
    };

    bb_serialize_walk(&s_gated_container_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(0, s_rec_n);
}

// ---------------------------------------------------------------------------
// 11. bb_serialize_desc_find hit + miss
// ---------------------------------------------------------------------------

void test_bb_serialize_desc_find_hit(void)
{
    const bb_serialize_field_t *f = bb_serialize_desc_find(&s_flat_desc, "u");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("u", f->key);
    TEST_ASSERT_EQUAL(BB_TYPE_U64, f->type);
}

void test_bb_serialize_desc_find_miss(void)
{
    TEST_ASSERT_NULL(bb_serialize_desc_find(&s_flat_desc, "nope"));
}

void test_bb_serialize_desc_find_null_desc(void)
{
    TEST_ASSERT_NULL(bb_serialize_desc_find(NULL, "i"));
}

void test_bb_serialize_desc_find_null_key(void)
{
    TEST_ASSERT_NULL(bb_serialize_desc_find(&s_flat_desc, NULL));
}

// ---------------------------------------------------------------------------
// 12. ARR-of-OBJ depth guard: self-referential element children bails at
// BB_SERIALIZE_MAX_DEPTH without recursing (distinct guard from the plain
// OBJ depth check -- covers the switch's BB_TYPE_ARR/elem_type==OBJ branch).
// ---------------------------------------------------------------------------

typedef struct {
    int64_t            marker;
    bb_serialize_arr_t kids;
} deep_arr_elem_t;

static const bb_serialize_field_t s_deep_arr_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_arr_elem_t, marker) },
    // Self-referential ARR-of-OBJ child: each element carries an array of
    // more elements of its own type, recursing forever unless the walker's
    // ARR-of-OBJ depth guard (distinct code path from the OBJ guard) bails.
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(deep_arr_elem_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(deep_arr_elem_t),
      .children = s_deep_arr_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_arr_desc = {
    .type_name = "deep_arr_elem_t",
    .fields = s_deep_arr_fields,
    .n_fields = 2,
    .snap_size = sizeof(deep_arr_elem_t),
};

void test_bb_serialize_arr_of_obj_depth_guard_bails_on_self_reference(void)
{
    rec_reset();

    // Build a chain of BB_SERIALIZE_MAX_DEPTH+1 nested single-element arrays
    // by hand (no heap): each level's `kids` array points at the next.
    deep_arr_elem_t levels[BB_SERIALIZE_MAX_DEPTH + 2];
    for (unsigned i = 0; i < BB_SERIALIZE_MAX_DEPTH + 2; i++) {
        levels[i].marker = (int64_t)i;
        levels[i].kids.items = NULL;
        levels[i].kids.count = 0;
    }
    for (unsigned i = 0; i < BB_SERIALIZE_MAX_DEPTH + 1; i++) {
        levels[i].kids.items = &levels[i + 1];
        levels[i].kids.count = 1;
    }

    bb_serialize_walk(&s_deep_arr_desc, &levels[0], &s_mock_emit);

    // At depth == BB_SERIALIZE_MAX_DEPTH the ARR-of-OBJ guard bails before
    // descending into the element loop -- no crash, no unbounded recursion.
    TEST_ASSERT_TRUE(s_rec_n > 0);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    TEST_ASSERT_EQUAL_INT64(0, s_rec[0].num.i64);
}

// ---------------------------------------------------------------------------
// 13. switch default: an out-of-range field type is a defensive no-op, not
// a crash (exhaustive-enum safety branch, hit via a deliberately invalid
// descriptor rather than removed).
// ---------------------------------------------------------------------------

void test_bb_serialize_unknown_type_is_noop(void)
{
    rec_reset();
    flat_snap_t snap = { .i = 1, .u = 2, .f = 3.0, .b = true };

    static const bb_serialize_field_t s_unknown_type_fields[] = {
        { .key = "bad", .type = (bb_type_t)99, .offset = 0 },
    };
    static const bb_serialize_desc_t s_unknown_type_desc = {
        .type_name = "flat_snap_t",
        .fields = s_unknown_type_fields,
        .n_fields = 1,
        .snap_size = sizeof(flat_snap_t),
    };

    bb_serialize_walk(&s_unknown_type_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(0, s_rec_n);
}

// ---------------------------------------------------------------------------
// 14. bb_serialize_desc_find skips a field with a NULL key (e.g. an
// array-element-only descriptor entry) rather than dereferencing it.
// ---------------------------------------------------------------------------

void test_bb_serialize_desc_find_skips_null_key_field(void)
{
    static const bb_serialize_field_t s_null_key_fields[] = {
        { .key = NULL, .type = BB_TYPE_I64, .offset = 0 },
        { .key = "found", .type = BB_TYPE_I64, .offset = 0 },
    };
    static const bb_serialize_desc_t s_null_key_desc = {
        .type_name = "null_key_snap_t",
        .fields = s_null_key_fields,
        .n_fields = 2,
        .snap_size = 0,
    };

    const bb_serialize_field_t *f = bb_serialize_desc_find(&s_null_key_desc, "found");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("found", f->key);
}

// ---------------------------------------------------------------------------
// Defensive: NULL args to bb_serialize_walk are a no-op, not a crash
// ---------------------------------------------------------------------------

void test_bb_serialize_walk_null_args_is_noop(void)
{
    rec_reset();
    flat_snap_t snap = { 0 };

    bb_serialize_walk(NULL, &snap, &s_mock_emit);
    bb_serialize_walk(&s_flat_desc, NULL, &s_mock_emit);
    bb_serialize_walk(&s_flat_desc, &snap, NULL);

    TEST_ASSERT_EQUAL_UINT(0, s_rec_n);
}

// ---------------------------------------------------------------------------
// bb_serialize_query_get -- request-scoped query-param carrier (relocated
// from bb_data so a filtered producer can depend on bb_serialize alone).
// ---------------------------------------------------------------------------

void test_bb_serialize_query_get_returns_matching_value(void)
{
    bb_serialize_query_t q = {
        .params = { { .key = "type", .value = "raw" }, { .key = "n", .value = "5" } },
        .count  = 2,
    };
    TEST_ASSERT_EQUAL_STRING("raw", bb_serialize_query_get(&q, "type"));
    TEST_ASSERT_EQUAL_STRING("5", bb_serialize_query_get(&q, "n"));
}

void test_bb_serialize_query_get_missing_key_returns_null(void)
{
    bb_serialize_query_t q = {
        .params = { { .key = "type", .value = "raw" } },
        .count  = 1,
    };
    TEST_ASSERT_NULL(bb_serialize_query_get(&q, "nope"));
}

void test_bb_serialize_query_get_null_query_returns_null(void)
{
    TEST_ASSERT_NULL(bb_serialize_query_get(NULL, "type"));
}

void test_bb_serialize_query_get_null_key_returns_null(void)
{
    bb_serialize_query_t q = {
        .params = { { .key = "type", .value = "raw" } },
        .count  = 1,
    };
    TEST_ASSERT_NULL(bb_serialize_query_get(&q, NULL));
}

// A caller-supplied count exceeding the fixed params[] capacity (e.g. a
// future HTTP query-string parser feeding untrusted input) must be clamped
// to BB_SERIALIZE_QUERY_MAX_PARAMS -- never trusted past the array's actual
// bound. Asserts a defined result (no OOB read/crash under ASan/valgrind)
// and that only the first MAX_PARAMS entries are ever consulted: a key
// that would only exist "past" the clamp is correctly reported missing.
void test_bb_serialize_query_get_count_over_capacity_is_clamped(void)
{
    bb_serialize_query_t q = {
        .params = { { .key = "a", .value = "1" }, { .key = "b", .value = "2" },
                    { .key = "c", .value = "3" }, { .key = "d", .value = "4" } },
        .count  = BB_SERIALIZE_QUERY_MAX_PARAMS + 100,  // deliberately over capacity
    };
    TEST_ASSERT_EQUAL_STRING("1", bb_serialize_query_get(&q, "a"));
    TEST_ASSERT_EQUAL_STRING("4", bb_serialize_query_get(&q, "d"));
    // Never reads past params[BB_SERIALIZE_QUERY_MAX_PARAMS - 1] regardless
    // of the inflated count -- a key that isn't actually present is still
    // correctly reported missing, not an OOB match.
    TEST_ASSERT_NULL(bb_serialize_query_get(&q, "nope"));
}
