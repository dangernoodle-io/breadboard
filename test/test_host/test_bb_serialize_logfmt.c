#include "unity.h"
#include "bb_serialize_logfmt.h"
#include "bb_serialize_format.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// bb_serialize_logfmt_render -- golden "key=value key=value" line + every
// scalar emit path, parity with bb_serialize_json's field-type coverage.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t               i;
    uint64_t              u;
    double                f;
    bool                  b;
    bb_serialize_str_n_t  s;
} logfmt_flat_snap_t;

static const bb_serialize_field_t s_logfmt_flat_fields[] = {
    { .key = "i", .type = BB_TYPE_I64, .offset = offsetof(logfmt_flat_snap_t, i) },
    { .key = "u", .type = BB_TYPE_U64, .offset = offsetof(logfmt_flat_snap_t, u) },
    { .key = "f", .type = BB_TYPE_F64, .offset = offsetof(logfmt_flat_snap_t, f) },
    { .key = "b", .type = BB_TYPE_BOOL, .offset = offsetof(logfmt_flat_snap_t, b) },
    { .key = "s", .type = BB_TYPE_STR_N, .offset = offsetof(logfmt_flat_snap_t, s), .max_len = 16 },
};

static const bb_serialize_desc_t s_logfmt_flat_desc = {
    .type_name = "logfmt_flat_snap_t",
    .fields = s_logfmt_flat_fields,
    .n_fields = 5,
    .snap_size = sizeof(logfmt_flat_snap_t),
};

void test_bb_serialize_logfmt_flat_scalars(void)
{
    logfmt_flat_snap_t snap = {
        .i = -7, .u = 42, .f = 1.5, .b = true, .s = { .ptr = "hi", .len = 2 },
    };
    char buf[128];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("i=-7 u=42 f=1.500000 b=true s=hi", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

void test_bb_serialize_logfmt_null_str_n_emits_null(void)
{
    logfmt_flat_snap_t snap = {
        .i = 0, .u = 0, .f = 0.0, .b = false, .s = { .ptr = NULL, .len = 0 },
    };
    char buf[128];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("i=0 u=0 f=0.000000 b=false s=null", buf);
}

// Empty string values are quoted ("") -- distinguishes "present, empty"
// from the unquoted `null` literal above (parity with the JSON backend's
// `""` vs `null` distinction).
void test_bb_serialize_logfmt_empty_str_n_is_quoted(void)
{
    logfmt_flat_snap_t snap = {
        .i = 0, .u = 0, .f = 0.0, .b = false, .s = { .ptr = "", .len = 0 },
    };
    char buf[128];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("i=0 u=0 f=0.000000 b=false s=\"\"", buf);
}

// ---------------------------------------------------------------------------
// f64 formatting edge cases -- same algorithm/coverage shape as
// bb_serialize_json.c's bb_json_write_f64() (NaN/Inf/out-of-range -> `null`,
// rounding-carry into the integer part).
// ---------------------------------------------------------------------------

typedef struct {
    double f;
} logfmt_f64_snap_t;

static const bb_serialize_field_t s_logfmt_f64_fields[] = {
    { .key = "f", .type = BB_TYPE_F64, .offset = offsetof(logfmt_f64_snap_t, f) },
};

static const bb_serialize_desc_t s_logfmt_f64_desc = {
    .type_name = "logfmt_f64_snap_t",
    .fields = s_logfmt_f64_fields,
    .n_fields = 1,
    .snap_size = sizeof(logfmt_f64_snap_t),
};

static void render_f64(double v, char *buf, size_t cap)
{
    logfmt_f64_snap_t snap = { .f = v };
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_f64_desc, &snap, buf, cap, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

void test_bb_serialize_logfmt_f64_nan_emits_null(void)
{
    char buf[64];
    render_f64(NAN, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("f=null", buf);
}

void test_bb_serialize_logfmt_f64_pos_inf_emits_null(void)
{
    char buf[64];
    render_f64(INFINITY, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("f=null", buf);
}

void test_bb_serialize_logfmt_f64_neg_inf_emits_null(void)
{
    char buf[64];
    render_f64(-INFINITY, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("f=null", buf);
}

void test_bb_serialize_logfmt_f64_out_of_u64_range_emits_null(void)
{
    char buf[64];
    render_f64(1.0e30, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("f=null", buf);
}

void test_bb_serialize_logfmt_f64_rounding_carry_into_integer_part(void)
{
    char buf[64];
    render_f64(0.9999996, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("f=1.000000", buf);
}

void test_bb_serialize_logfmt_f64_negative_value(void)
{
    char buf[64];
    render_f64(-2.25, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("f=-2.250000", buf);
}

// ---------------------------------------------------------------------------
// Quoting/escaping edge cases -- the logfmt-specific behavior.
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_str_n_t v;
} logfmt_str_snap_t;

static const bb_serialize_field_t s_logfmt_str_fields[] = {
    { .key = "v", .type = BB_TYPE_STR_N, .offset = offsetof(logfmt_str_snap_t, v), .max_len = 64 },
};

static const bb_serialize_desc_t s_logfmt_str_desc = {
    .type_name = "logfmt_str_snap_t",
    .fields = s_logfmt_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(logfmt_str_snap_t),
};

static void render_str(const char *s, size_t len, char *buf, size_t cap)
{
    logfmt_str_snap_t snap = { .v = { .ptr = s, .len = len } };
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_str_desc, &snap, buf, cap, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

void test_bb_serialize_logfmt_plain_value_unquoted(void)
{
    char buf[64];
    render_str("hello", 5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("v=hello", buf);
}

void test_bb_serialize_logfmt_value_with_space_is_quoted(void)
{
    char buf[64];
    render_str("hello world", 11, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("v=\"hello world\"", buf);
}

void test_bb_serialize_logfmt_value_with_equals_is_quoted(void)
{
    char buf[64];
    render_str("a=b", 3, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("v=\"a=b\"", buf);
}

void test_bb_serialize_logfmt_value_with_quote_is_escaped(void)
{
    char buf[64];
    render_str("say \"hi\"", 8, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("v=\"say \\\"hi\\\"\"", buf);
}

// Quote-without-space: bb_logfmt_needs_quote()'s `c == '"'` check is only
// exercised (both directions) when no earlier byte (e.g. a space) already
// short-circuits the OR chain -- this drives that branch directly.
void test_bb_serialize_logfmt_value_with_quote_only_no_space_is_escaped(void)
{
    char buf[64];
    render_str("a\"b", 3, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("v=\"a\\\"b\"", buf);
}

void test_bb_serialize_logfmt_value_with_backslash_is_escaped(void)
{
    char buf[64];
    render_str("a\\b", 3, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("v=\"a\\\\b\"", buf);
}

void test_bb_serialize_logfmt_value_with_newline_tab_cr_escaped(void)
{
    char buf[64];
    render_str("a\nb\tc\rd", 7, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("v=\"a\\nb\\tc\\rd\"", buf);
}

void test_bb_serialize_logfmt_value_with_other_control_byte_hex_escaped(void)
{
    char buf[64];
    char s[1] = { (char)0x01 };
    render_str(s, 1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("v=\"\\x01\"", buf);
}

// ---------------------------------------------------------------------------
// Nested OBJ/ARR: structurally safe (never crashes), begin/end are no-ops --
// same convention as bb_serialize_console.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} logfmt_inner_t;

typedef struct {
    logfmt_inner_t o;
} logfmt_obj_snap_t;

static const bb_serialize_field_t s_logfmt_inner_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(logfmt_inner_t, n) },
};

static const bb_serialize_field_t s_logfmt_obj_fields[] = {
    { .key = "o", .type = BB_TYPE_OBJ, .offset = offsetof(logfmt_obj_snap_t, o),
      .children = s_logfmt_inner_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_logfmt_obj_desc = {
    .type_name = "logfmt_obj_snap_t",
    .fields = s_logfmt_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(logfmt_obj_snap_t),
};

void test_bb_serialize_logfmt_nested_obj_no_crash(void)
{
    logfmt_obj_snap_t snap = { .o = { .n = 5 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_obj_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("n=5", buf);
}

typedef struct {
    bb_serialize_arr_t a;
} logfmt_arr_snap_t;

static const bb_serialize_field_t s_logfmt_arr_fields[] = {
    { .key = "a", .type = BB_TYPE_ARR, .offset = offsetof(logfmt_arr_snap_t, a),
      .elem_type = BB_TYPE_STR, .max_len = 8, .max_items = 4 },
};

static const bb_serialize_desc_t s_logfmt_arr_desc = {
    .type_name = "logfmt_arr_snap_t",
    .fields = s_logfmt_arr_fields,
    .n_fields = 1,
    .snap_size = sizeof(logfmt_arr_snap_t),
};

void test_bb_serialize_logfmt_array_of_strings_no_crash(void)
{
    const char *items[] = { "x", "y" };
    logfmt_arr_snap_t snap = { .a = { .items = items, .count = 2 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_arr_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("x y", buf);
}

// ---------------------------------------------------------------------------
// All-or-nothing overflow contract -- parity with bb_serialize_json_render(),
// NOT bb_serialize_console_render()'s truncation contract.
// ---------------------------------------------------------------------------

void test_bb_serialize_logfmt_render_overflow_fails_all_or_nothing(void)
{
    logfmt_flat_snap_t snap = {
        .i = 123456789, .u = 42, .f = 1.5, .b = true, .s = { .ptr = "hi", .len = 2 },
    };
    char buf[6] = { 'X', 'X', 'X', 'X', 'X', 'X' };
    size_t out_len = 123;

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT(0, out_len);
    TEST_ASSERT_EQUAL_STRING("", buf);  // NUL'd, never partial content
}

// Overflow (ctx.err != BB_OK) with out_len == NULL -- distinct from the
// cap==0/buf==NULL early-reject path, which has its own out_len-NULL branch;
// this drives the *walk-produced-an-error* branch's out_len-NULL side.
void test_bb_serialize_logfmt_render_overflow_null_out_len(void)
{
    logfmt_flat_snap_t snap = {
        .i = 123456789, .u = 42, .f = 1.5, .b = true, .s = { .ptr = "hi", .len = 2 },
    };
    char buf[6] = { 'X', 'X', 'X', 'X', 'X', 'X' };

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);  // NUL'd, never partial content
}

void test_bb_serialize_logfmt_render_rejects_null_buf(void)
{
    logfmt_flat_snap_t snap = { 0 };
    size_t out_len = 123;

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, NULL, 16, &out_len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT(0, out_len);
}

void test_bb_serialize_logfmt_render_rejects_zero_cap(void)
{
    logfmt_flat_snap_t snap = { 0 };
    char buf[16] = { 'X' };
    size_t out_len = 123;

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, buf, 0, &out_len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT(0, out_len);
    TEST_ASSERT_EQUAL('X', buf[0]);  // untouched
}

void test_bb_serialize_logfmt_render_rejects_zero_cap_null_out_len(void)
{
    logfmt_flat_snap_t snap = { 0 };
    char buf[16] = { 'X' };

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, buf, 0, NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL('X', buf[0]);  // untouched
}

void test_bb_serialize_logfmt_render_success_null_out_len(void)
{
    logfmt_flat_snap_t snap = {
        .i = 1, .u = 2, .f = 3.0, .b = true, .s = { .ptr = "z", .len = 1 },
    };
    char buf[64];

    bb_err_t rc = bb_serialize_logfmt_render(&s_logfmt_flat_desc, &snap, buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("i=1 u=2 f=3.000000 b=true s=z", buf);
}

// ---------------------------------------------------------------------------
// bb_serialize_logfmt_ctx_init / bb_serialize_logfmt_emit -- direct-drive
// path (bypassing bb_serialize_logfmt_render()).
// ---------------------------------------------------------------------------

void test_bb_serialize_logfmt_ctx_init_empty(void)
{
    char buf[16];
    bb_serialize_logfmt_ctx_t ctx;

    bb_serialize_logfmt_ctx_init(&ctx, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(0, ctx.len);
    TEST_ASSERT_EQUAL_UINT(sizeof(buf), ctx.cap);
    TEST_ASSERT_EQUAL(BB_OK, ctx.err);
}

void test_bb_serialize_logfmt_emit_direct_drive(void)
{
    typedef struct { int64_t n; } zc_snap_t;
    static const bb_serialize_field_t fields[] = {
        { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(zc_snap_t, n) },
    };
    static const bb_serialize_desc_t desc = {
        .type_name = "zc_snap_t", .fields = fields, .n_fields = 1, .snap_size = sizeof(zc_snap_t),
    };
    zc_snap_t snap = { .n = 7 };

    char buf[16];
    bb_serialize_logfmt_ctx_t ctx;
    bb_serialize_logfmt_ctx_init(&ctx, buf, sizeof(buf));

    bb_serialize_emit_t emit = bb_serialize_logfmt_emit(&ctx);
    TEST_ASSERT_EQUAL(BB_FORMAT_LOGFMT, emit.format_id);
    bb_serialize_walk(&desc, &snap, &emit);

    buf[ctx.len] = '\0';
    TEST_ASSERT_EQUAL_STRING("n=7", buf);
}

// ---------------------------------------------------------------------------
// bb_serialize_logfmt_register_format -- registry round trip + idempotency.
// ---------------------------------------------------------------------------

void test_bb_serialize_logfmt_register_format_idempotent(void)
{
    bb_serialize_format_test_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_logfmt_register_format());
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_logfmt_register_format());

    bb_serialize_render_fn render = bb_serialize_format_get_render(BB_FORMAT_LOGFMT);
    TEST_ASSERT_NOT_NULL(render);
    TEST_ASSERT_NULL(bb_serialize_format_get_parse(BB_FORMAT_LOGFMT));

    typedef struct { int64_t n; } rt_snap_t;
    static const bb_serialize_field_t rt_fields[] = {
        { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(rt_snap_t, n) },
    };
    static const bb_serialize_desc_t rt_desc = {
        .type_name = "rt_snap_t", .fields = rt_fields, .n_fields = 1, .snap_size = sizeof(rt_snap_t),
    };
    rt_snap_t rt = { .n = 7 };
    char buf[32];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, render(&rt_desc, &rt, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("n=7", buf);
}

// ---------------------------------------------------------------------------
// bb_format_name -- BB_FORMAT_LOGFMT coverage (BB_FORMAT_JSON/CONSOLE/NONE/
// out-of-range already covered by test_bb_serialize_format.c and
// test_bb_serialize_console.c).
// ---------------------------------------------------------------------------

void test_bb_format_name_logfmt_returns_logfmt(void)
{
    TEST_ASSERT_EQUAL_STRING("logfmt", bb_format_name(BB_FORMAT_LOGFMT));
}
