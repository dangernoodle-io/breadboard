#include "unity.h"
#include "bb_serialize_json.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Golden byte-identity tests -- each fixture renders via
// bb_serialize_json_render() and asserts the exact resulting string.
// ---------------------------------------------------------------------------

// 1. flat scalars ------------------------------------------------------

typedef struct {
    int64_t               a;
    bool                  b;
    bb_serialize_str_n_t  c;
} flat_snap2_t;

static const bb_serialize_field_t s_flat2_fields[] = {
    { .key = "a", .type = BB_TYPE_I64, .offset = offsetof(flat_snap2_t, a) },
    { .key = "b", .type = BB_TYPE_BOOL, .offset = offsetof(flat_snap2_t, b) },
    { .key = "c", .type = BB_TYPE_STR_N, .offset = offsetof(flat_snap2_t, c), .max_len = 8 },
};

static const bb_serialize_desc_t s_flat2_desc = {
    .type_name = "flat_snap2_t",
    .fields = s_flat2_fields,
    .n_fields = 3,
    .snap_size = sizeof(flat_snap2_t),
};

void test_bb_serialize_json_flat_scalars(void)
{
    flat_snap2_t snap = { .a = 1, .b = true, .c = { .ptr = "x", .len = 1 } };
    char buf[128];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_flat2_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1,\"b\":true,\"c\":\"x\"}", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
    TEST_ASSERT_TRUE(out_len <= bb_serialize_json_bound(&s_flat2_desc));
}

// 2. nested OBJ + empty OBJ ---------------------------------------------

typedef struct {
    int64_t n;
} inner_t;

typedef struct {
    inner_t o;
} obj_snap_t;

static const bb_serialize_field_t s_inner_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(inner_t, n) },
};

static const bb_serialize_field_t s_obj_fields[] = {
    { .key = "o", .type = BB_TYPE_OBJ, .offset = offsetof(obj_snap_t, o),
      .children = s_inner_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_obj_desc = {
    .type_name = "obj_snap_t",
    .fields = s_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(obj_snap_t),
};

void test_bb_serialize_json_nested_obj(void)
{
    obj_snap_t snap = { .o = { .n = 5 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_obj_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"o\":{\"n\":5}}", buf);
    TEST_ASSERT_TRUE(out_len <= bb_serialize_json_bound(&s_obj_desc));
}

static const bb_serialize_field_t s_empty_obj_fields[] = {
    { .key = "o", .type = BB_TYPE_OBJ, .offset = offsetof(obj_snap_t, o),
      .children = NULL, .n_children = 0 },
};

static const bb_serialize_desc_t s_empty_obj_desc = {
    .type_name = "obj_snap_t",
    .fields = s_empty_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(obj_snap_t),
};

void test_bb_serialize_json_empty_obj(void)
{
    obj_snap_t snap = { .o = { .n = 5 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_empty_obj_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"o\":{}}", buf);
    TEST_ASSERT_TRUE(out_len <= bb_serialize_json_bound(&s_empty_obj_desc));
}

// 3. array of strings, empty array --------------------------------------

typedef struct {
    bb_serialize_arr_t a;
} arr_str_snap_t;

static const bb_serialize_field_t s_arr_str_fields[] = {
    { .key = "a", .type = BB_TYPE_ARR, .offset = offsetof(arr_str_snap_t, a),
      .elem_type = BB_TYPE_STR, .max_len = 8, .max_items = 4 },
};

static const bb_serialize_desc_t s_arr_str_desc = {
    .type_name = "arr_str_snap_t",
    .fields = s_arr_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_str_snap_t),
};

void test_bb_serialize_json_array_of_strings(void)
{
    const char *items[] = { "x", "y" };
    arr_str_snap_t snap = { .a = { .items = items, .count = 2 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_arr_str_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"a\":[\"x\",\"y\"]}", buf);
    TEST_ASSERT_TRUE(out_len <= bb_serialize_json_bound(&s_arr_str_desc));
}

void test_bb_serialize_json_array_empty(void)
{
    arr_str_snap_t snap = { .a = { .items = NULL, .count = 0 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_arr_str_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"a\":[]}", buf);
}

// 4. array of OBJ ---------------------------------------------------------

typedef struct {
    int64_t n;
} arr_elem_t;

typedef struct {
    bb_serialize_arr_t a;
} arr_obj_snap_t;

static const bb_serialize_field_t s_arr_elem_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(arr_elem_t, n) },
};

static const bb_serialize_field_t s_arr_obj_fields[] = {
    { .key = "a", .type = BB_TYPE_ARR, .offset = offsetof(arr_obj_snap_t, a),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(arr_elem_t), .max_items = 4,
      .children = s_arr_elem_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_arr_obj_desc = {
    .type_name = "arr_obj_snap_t",
    .fields = s_arr_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_obj_snap_t),
};

void test_bb_serialize_json_array_of_obj(void)
{
    arr_elem_t elems[2] = { { .n = 1 }, { .n = 2 } };
    arr_obj_snap_t snap = { .a = { .items = elems, .count = 2 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_arr_obj_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"a\":[{\"n\":1},{\"n\":2}]}", buf);
    TEST_ASSERT_TRUE(out_len <= bb_serialize_json_bound(&s_arr_obj_desc));
}

// 5. present-gate omit ------------------------------------------------

typedef struct {
    int64_t a;
    int64_t b;
} gated_snap_t;

static bool gate_a_absent(const void *snap) { (void)snap; return false; }

static const bb_serialize_field_t s_gated_fields[] = {
    { .key = "a", .type = BB_TYPE_I64, .offset = offsetof(gated_snap_t, a), .present = gate_a_absent },
    { .key = "b", .type = BB_TYPE_I64, .offset = offsetof(gated_snap_t, b) },
};

static const bb_serialize_desc_t s_gated_desc = {
    .type_name = "gated_snap_t",
    .fields = s_gated_fields,
    .n_fields = 2,
    .snap_size = sizeof(gated_snap_t),
};

void test_bb_serialize_json_present_gate_omits_field(void)
{
    gated_snap_t snap = { .a = 1, .b = 2 };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_gated_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"b\":2}", buf);
    TEST_ASSERT_TRUE(out_len <= bb_serialize_json_bound(&s_gated_desc));
}

// 6. escaping -----------------------------------------------------------

typedef struct {
    bb_serialize_str_n_t s;
} str_snap_t;

static const bb_serialize_field_t s_str_fields[] = {
    { .key = "s", .type = BB_TYPE_STR_N, .offset = offsetof(str_snap_t, s) },
};

static const bb_serialize_desc_t s_str_desc = {
    .type_name = "str_snap_t",
    .fields = s_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(str_snap_t),
};

static void render_str(const char *raw, size_t raw_len, char *buf, size_t cap)
{
    str_snap_t snap = { .s = { .ptr = raw, .len = raw_len } };
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&s_str_desc, &snap, buf, cap, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

void test_bb_serialize_json_escape_quote_and_backslash(void)
{
    char buf[64];
    render_str("a\"b\\c", 5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"a\\\"b\\\\c\"}", buf);
}

void test_bb_serialize_json_escape_control_chars(void)
{
    char buf[64];
    render_str("\n\r\t\b\f", 5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"\\n\\r\\t\\b\\f\"}", buf);
}

void test_bb_serialize_json_escape_low_control_byte(void)
{
    char buf[64];
    char raw[1] = { 0x01 };
    render_str(raw, 1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"\\u0001\"}", buf);
}

void test_bb_serialize_json_escape_low_control_byte_hex_letter_digit(void)
{
    // 0x1F's low nibble (0xF) exercises the >= 10 branch of the hex-nibble
    // helper (produces 'a'-'f'), distinct from the low-control test above
    // (0x01, both nibbles < 10).
    char buf[64];
    char raw[1] = { 0x1F };
    render_str(raw, 1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"\\u001f\"}", buf);
}

void test_bb_serialize_json_forward_slash_not_escaped(void)
{
    char buf[64];
    render_str("a/b", 3, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"a/b\"}", buf);
}

void test_bb_serialize_json_utf8_multibyte_passthrough(void)
{
    char buf[64];
    // U+00E9 ("e" with acute accent), UTF-8 encoded: 0xC3 0xA9.
    const char raw[2] = { (char)0xC3, (char)0xA9 };
    render_str(raw, 2, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"\xC3\xA9\"}", buf);
}

// 7. STR strnlen bound (embedded char[N], not NUL-terminated) -----------

typedef struct {
    char status[4];
} embedded_str_snap_t;

static const bb_serialize_field_t s_embedded_str_fields[] = {
    { .key = "st", .type = BB_TYPE_STR, .offset = offsetof(embedded_str_snap_t, status), .max_len = 4 },
};

static const bb_serialize_desc_t s_embedded_str_desc = {
    .type_name = "embedded_str_snap_t",
    .fields = s_embedded_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(embedded_str_snap_t),
};

void test_bb_serialize_json_str_strnlen_bound_truncates(void)
{
    embedded_str_snap_t snap;
    memcpy(snap.status, "abcd", 4);  // not NUL-terminated within the 4-byte bound
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_embedded_str_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"st\":\"abcd\"}", buf);
}

// 8. STR_N NULL ptr -> null; len 0 non-NULL -> "" ------------------------

void test_bb_serialize_json_str_n_null_ptr_emits_null(void)
{
    str_snap_t snap = { .s = { .ptr = NULL, .len = 5 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_str_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"s\":null}", buf);
}

void test_bb_serialize_json_str_n_empty_non_null_emits_empty_string(void)
{
    str_snap_t snap = { .s = { .ptr = "", .len = 0 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_str_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"\"}", buf);
}

// 9. integers: i64 negative + INT64_MIN, u64 max -------------------------

typedef struct {
    int64_t  i;
    uint64_t u;
} int_snap_t;

static const bb_serialize_field_t s_int_fields[] = {
    { .key = "i", .type = BB_TYPE_I64, .offset = offsetof(int_snap_t, i) },
    { .key = "u", .type = BB_TYPE_U64, .offset = offsetof(int_snap_t, u) },
};

static const bb_serialize_desc_t s_int_desc = {
    .type_name = "int_snap_t",
    .fields = s_int_fields,
    .n_fields = 2,
    .snap_size = sizeof(int_snap_t),
};

void test_bb_serialize_json_i64_negative(void)
{
    int_snap_t snap = { .i = -42, .u = 0 };
    char buf[64];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_render(&s_int_desc, &snap, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"i\":-42,\"u\":0}", buf);
}

void test_bb_serialize_json_i64_min(void)
{
    int_snap_t snap = { .i = INT64_MIN, .u = 0 };
    char buf[64];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_render(&s_int_desc, &snap, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"i\":-9223372036854775808,\"u\":0}", buf);
}

void test_bb_serialize_json_u64_max(void)
{
    int_snap_t snap = { .i = 0, .u = UINT64_MAX };
    char buf[64];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_render(&s_int_desc, &snap, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"i\":0,\"u\":18446744073709551615}", buf);
}

// 10. f64 formatting ------------------------------------------------------

typedef struct {
    double f;
} f64_snap_t;

static const bb_serialize_field_t s_f64_fields[] = {
    { .key = "f", .type = BB_TYPE_F64, .offset = offsetof(f64_snap_t, f) },
};

static const bb_serialize_desc_t s_f64_desc = {
    .type_name = "f64_snap_t",
    .fields = s_f64_fields,
    .n_fields = 1,
    .snap_size = sizeof(f64_snap_t),
};

static void render_f64(double v, char *buf, size_t cap)
{
    f64_snap_t snap = { .f = v };
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&s_f64_desc, &snap, buf, cap, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_TRUE(out_len <= bb_serialize_json_bound(&s_f64_desc));
}

void test_bb_serialize_json_f64_simple(void)
{
    char buf[64];
    render_f64(1.5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":1.500000}", buf);
}

void test_bb_serialize_json_f64_negative_zero(void)
{
    char buf[64];
    render_f64(-0.0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":0.000000}", buf);
}

void test_bb_serialize_json_f64_integer_valued(void)
{
    char buf[64];
    render_f64(3.0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":3.000000}", buf);
}

void test_bb_serialize_json_f64_nan_emits_null(void)
{
    char buf[64];
    render_f64(NAN, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":null}", buf);
}

void test_bb_serialize_json_f64_pos_inf_emits_null(void)
{
    char buf[64];
    render_f64(INFINITY, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":null}", buf);
}

void test_bb_serialize_json_f64_neg_inf_emits_null(void)
{
    char buf[64];
    render_f64(-INFINITY, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":null}", buf);
}

void test_bb_serialize_json_f64_rounding_carry_into_integer_part(void)
{
    char buf[64];
    render_f64(0.9999996, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":1.000000}", buf);
}

void test_bb_serialize_json_f64_negative_value(void)
{
    char buf[64];
    render_f64(-2.25, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":-2.250000}", buf);
}

void test_bb_serialize_json_f64_out_of_u64_range_emits_null(void)
{
    char buf[64];
    render_f64(1.0e30, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"f\":null}", buf);
}

// 11. bool false, null ----------------------------------------------------

typedef struct {
    bool b;
} bool_snap_t;

static const bb_serialize_field_t s_bool_fields[] = {
    { .key = "b", .type = BB_TYPE_BOOL, .offset = offsetof(bool_snap_t, b) },
};

static const bb_serialize_desc_t s_bool_desc = {
    .type_name = "bool_snap_t",
    .fields = s_bool_fields,
    .n_fields = 1,
    .snap_size = sizeof(bool_snap_t),
};

void test_bb_serialize_json_bool_false(void)
{
    bool_snap_t snap = { .b = false };
    char buf[64];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_render(&s_bool_desc, &snap, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"b\":false}", buf);
}

// 12. overflow: too-small cap -> BB_ERR_NO_SPACE, no scribble past cap ----

void test_bb_serialize_json_overflow_too_small_cap(void)
{
    char buf[6];  // {"a":1  -- not enough room for the closing brace + NUL
    buf[5] = (char)0xAA;  // sentinel: index cap-1, must stay untouched
    size_t out_len = 123;

    bb_err_t rc = bb_serialize_json_render(&s_int_desc, &(int_snap_t){ .i = 1, .u = 0 }, buf, 5, &out_len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT(0, out_len);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, (unsigned char)buf[5]);
}

void test_bb_serialize_json_render_cap_zero(void)
{
    char buf[1] = { (char)0xAA };
    size_t out_len = 123;

    bb_err_t rc = bb_serialize_json_render(&s_int_desc, &(int_snap_t){ .i = 1, .u = 0 }, buf, 0, &out_len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    // cap==0 bails before touching buf/out_len -- out_len untouched.
    TEST_ASSERT_EQUAL_UINT(123, out_len);
    TEST_ASSERT_EQUAL_HEX8(0xAA, (unsigned char)buf[0]);
}

void test_bb_serialize_json_render_null_out_len_on_success(void)
{
    int_snap_t snap = { .i = 1, .u = 2 };
    char buf[64];

    bb_err_t rc = bb_serialize_json_render(&s_int_desc, &snap, buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"i\":1,\"u\":2}", buf);
}

void test_bb_serialize_json_render_null_out_len_on_overflow(void)
{
    char buf[4];
    bb_err_t rc = bb_serialize_json_render(&s_int_desc, &(int_snap_t){ .i = 1, .u = 0 }, buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// 13. put() no-op when err already set (covers the sticky-error branch
// beyond the byte that first triggers it -- overflow mid-escape-sequence).
void test_bb_serialize_json_overflow_mid_string_stays_sticky(void)
{
    // "s" key + colon + opening quote alone exceeds a 4-byte cap, so the
    // very first pre_value()/emit_str() call overflows and every
    // subsequent put() (including the multi-byte escape writes) must be a
    // no-op against the already-sticky error, not a partial scribble.
    char buf[4];
    str_snap_t snap = { .s = { .ptr = "\n\"\\", .len = 3 } };
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_str_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT(0, out_len);
}

// 14. bb_serialize_json_bound ----------------------------------------------

void test_bb_serialize_json_bound_flat(void)
{
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, bb_serialize_json_bound(&s_flat2_desc));
}

void test_bb_serialize_json_bound_null_desc(void)
{
    TEST_ASSERT_EQUAL_UINT(0, bb_serialize_json_bound(NULL));
}

typedef struct {
    bb_serialize_str_n_t s;
} unbounded_str_snap_t;

static const bb_serialize_field_t s_unbounded_str_fields[] = {
    // max_len == 0 -> unbounded (STR_N with no width hint).
    { .key = "s", .type = BB_TYPE_STR_N, .offset = offsetof(unbounded_str_snap_t, s), .max_len = 0 },
};

static const bb_serialize_desc_t s_unbounded_str_desc = {
    .type_name = "unbounded_str_snap_t",
    .fields = s_unbounded_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(unbounded_str_snap_t),
};

void test_bb_serialize_json_bound_unbounded_str_n_is_size_max(void)
{
    TEST_ASSERT_EQUAL_UINT(SIZE_MAX, bb_serialize_json_bound(&s_unbounded_str_desc));
}

typedef struct {
    bb_serialize_arr_t a;
} unbounded_arr_snap_t;

static const bb_serialize_field_t s_unbounded_arr_fields[] = {
    // max_items == 0 -> unbounded array.
    { .key = "a", .type = BB_TYPE_ARR, .offset = offsetof(unbounded_arr_snap_t, a),
      .elem_type = BB_TYPE_STR, .max_len = 8, .max_items = 0 },
};

static const bb_serialize_desc_t s_unbounded_arr_desc = {
    .type_name = "unbounded_arr_snap_t",
    .fields = s_unbounded_arr_fields,
    .n_fields = 1,
    .snap_size = sizeof(unbounded_arr_snap_t),
};

void test_bb_serialize_json_bound_unbounded_arr_is_size_max(void)
{
    TEST_ASSERT_EQUAL_UINT(SIZE_MAX, bb_serialize_json_bound(&s_unbounded_arr_desc));
}

// Array-of-OBJ whose element schema itself contains an unbounded field --
// propagates SIZE_MAX up through elem_bound()'s OBJ recursion branch.
typedef struct {
    bb_serialize_str_n_t s;
} unbounded_elem_t;

typedef struct {
    bb_serialize_arr_t a;
} unbounded_arr_obj_snap_t;

static const bb_serialize_field_t s_unbounded_elem_fields[] = {
    { .key = "s", .type = BB_TYPE_STR_N, .offset = offsetof(unbounded_elem_t, s), .max_len = 0 },
};

static const bb_serialize_field_t s_unbounded_arr_obj_fields[] = {
    { .key = "a", .type = BB_TYPE_ARR, .offset = offsetof(unbounded_arr_obj_snap_t, a),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(unbounded_elem_t), .max_items = 4,
      .children = s_unbounded_elem_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_unbounded_arr_obj_desc = {
    .type_name = "unbounded_arr_obj_snap_t",
    .fields = s_unbounded_arr_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(unbounded_arr_obj_snap_t),
};

void test_bb_serialize_json_bound_unbounded_arr_of_obj_element_is_size_max(void)
{
    TEST_ASSERT_EQUAL_UINT(SIZE_MAX, bb_serialize_json_bound(&s_unbounded_arr_obj_desc));
}

// Array of strings whose max_items IS set (nonzero) but whose element
// max_len is 0 -- distinct from test_bb_serialize_json_bound_unbounded_arr_
// is_size_max above, which hits the max_items==0 short-circuit before ever
// reaching bb_json_bound_elem()'s own STR/max_len==0 check.
typedef struct {
    bb_serialize_arr_t a;
} unbounded_arr_str_elem_snap_t;

static const bb_serialize_field_t s_unbounded_arr_str_elem_fields[] = {
    { .key = "a", .type = BB_TYPE_ARR, .offset = offsetof(unbounded_arr_str_elem_snap_t, a),
      .elem_type = BB_TYPE_STR, .max_len = 0, .max_items = 4 },
};

static const bb_serialize_desc_t s_unbounded_arr_str_elem_desc = {
    .type_name = "unbounded_arr_str_elem_snap_t",
    .fields = s_unbounded_arr_str_elem_fields,
    .n_fields = 1,
    .snap_size = sizeof(unbounded_arr_str_elem_snap_t),
};

void test_bb_serialize_json_bound_unbounded_arr_str_elem_max_len_is_size_max(void)
{
    TEST_ASSERT_EQUAL_UINT(SIZE_MAX, bb_serialize_json_bound(&s_unbounded_arr_str_elem_desc));
}

// Nested OBJ containing an unbounded field -- propagates SIZE_MAX up
// through bb_json_bound_value()'s BB_TYPE_OBJ branch.
typedef struct {
    unbounded_elem_t inner;
} unbounded_obj_snap_t;

static const bb_serialize_field_t s_unbounded_obj_fields[] = {
    { .key = "inner", .type = BB_TYPE_OBJ, .offset = offsetof(unbounded_obj_snap_t, inner),
      .children = s_unbounded_elem_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_unbounded_obj_desc = {
    .type_name = "unbounded_obj_snap_t",
    .fields = s_unbounded_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(unbounded_obj_snap_t),
};

void test_bb_serialize_json_bound_unbounded_nested_obj_is_size_max(void)
{
    TEST_ASSERT_EQUAL_UINT(SIZE_MAX, bb_serialize_json_bound(&s_unbounded_obj_desc));
}

// Depth-capped OBJ / ARR-of-OBJ: a self-referential descriptor exercises
// bb_json_bound_value()/bb_json_bound_elem()'s `depth >= BB_SERIALIZE_MAX_DEPTH`
// branches without an actual runtime stack overflow (bound() never
// recurses on live data, only on the static descriptor).
typedef struct {
    int64_t marker;
} deep_snap_t;

static const bb_serialize_field_t s_deep_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_snap_t, marker) },
    { .key = "deep", .type = BB_TYPE_OBJ, .offset = 0,
      .children = s_deep_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_desc = {
    .type_name = "deep_snap_t",
    .fields = s_deep_fields,
    .n_fields = 2,
    .snap_size = sizeof(deep_snap_t),
};

void test_bb_serialize_json_bound_depth_capped_self_reference(void)
{
    // Must terminate (not infinite-recurse) and produce a finite bound.
    size_t b = bb_serialize_json_bound(&s_deep_desc);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, b);
    TEST_ASSERT_TRUE(b > 0);
}

typedef struct {
    int64_t            marker;
    bb_serialize_arr_t kids;
} deep_arr_elem_t;

static const bb_serialize_field_t s_deep_arr_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_arr_elem_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(deep_arr_elem_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(deep_arr_elem_t), .max_items = 2,
      .children = s_deep_arr_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_arr_desc = {
    .type_name = "deep_arr_elem_t",
    .fields = s_deep_arr_fields,
    .n_fields = 2,
    .snap_size = sizeof(deep_arr_elem_t),
};

void test_bb_serialize_json_bound_depth_capped_arr_of_obj_self_reference(void)
{
    size_t b = bb_serialize_json_bound(&s_deep_arr_desc);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, b);
    TEST_ASSERT_TRUE(b > 0);
}

// 15. defensive: out-of-range field type in bound()'s value_max switch
// (exhaustive-enum safety branch, hit via a deliberately invalid
// descriptor rather than removed -- mirrors bb_serialize_walk's own
// equivalent defensive-branch test).
void test_bb_serialize_json_bound_unknown_type_defensive(void)
{
    static const bb_serialize_field_t s_unknown_type_fields[] = {
        { .key = "bad", .type = (bb_type_t)99, .offset = 0 },
    };
    static const bb_serialize_desc_t s_unknown_type_desc = {
        .type_name = "unknown",
        .fields = s_unknown_type_fields,
        .n_fields = 1,
        .snap_size = 0,
    };

    size_t b = bb_serialize_json_bound(&s_unknown_type_desc);
    // value_max() == 0 for the unknown type -> field_cost == key_cost + 0 + 1.
    TEST_ASSERT_EQUAL_UINT(2 + (6 * 3 + 3 + 0 + 1) + 1, b);
}

// 16. ctx_init / manual emit-vtable drive (direct API, not via render()) --
// covers bb_serialize_json_ctx_init()/bb_serialize_json_emit() used
// standalone, and the key==NULL defensive branch inside pre_value(): the
// walker itself never calls a callback with key==NULL while in an object
// (non-array) context (top-level/OBJ-member fields always carry a real
// key; only ARRAY elements pass NULL, where is_array is already true) --
// this scenario is reachable only by driving the vtable directly.
void test_bb_serialize_json_ctx_init_manual_drive_key_null_defensive(void)
{
    char buf[32];
    bb_serialize_json_ctx_t ctx;
    bb_serialize_json_ctx_init(&ctx, buf, sizeof(buf) - 1);
    bb_serialize_emit_t emit = bb_serialize_json_emit(&ctx);

    TEST_ASSERT_EQUAL(BB_FORMAT_JSON, emit.format_id);
    TEST_ASSERT_EQUAL_PTR(&ctx, emit.ctx);

    // Root context (stack[0]) is object (is_array=false) by construction.
    emit.emit_i64(emit.ctx, NULL, 7);
    buf[ctx.len] = '\0';

    TEST_ASSERT_EQUAL(BB_OK, ctx.err);
    TEST_ASSERT_EQUAL_STRING("\"\":7", buf);
}

// 17. deep-nesting render -- proves the writer's nesting stack (sized
// 2*BB_SERIALIZE_MAX_DEPTH+2 to cover an arr-of-obj field costing TWO
// stack frames per walker recursion level) never overflows on a real
// render, not just bb_serialize_json_bound() (a descriptor-only sizing
// helper that never drives the writer's stack at all). Before the fix
// (stack sized MAX_DEPTH+2, one frame per level) this arr-of-obj case
// wrote past the array -- ASAN-confirmed stack-buffer-overflow.

// 17a. plain OBJ nested to BB_SERIALIZE_MAX_DEPTH -- one stack frame per
// walker level, self-referential (offset=0 re-reads the same struct
// instance at every level, so the marker value is constant at every
// depth and the expected string is a simple, mechanically-generated
// nest of "deep" wrappers).
typedef struct {
    int64_t marker;
} deep_obj_render_snap_t;

static const bb_serialize_field_t s_deep_obj_render_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_obj_render_snap_t, marker) },
    { .key = "deep", .type = BB_TYPE_OBJ, .offset = 0,
      .children = s_deep_obj_render_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_obj_render_desc = {
    .type_name = "deep_obj_render_snap_t",
    .fields = s_deep_obj_render_fields,
    .n_fields = 2,
    .snap_size = sizeof(deep_obj_render_snap_t),
};

void test_bb_serialize_json_render_deep_obj(void)
{
    deep_obj_render_snap_t snap = { .marker = 42 };
    char buf[512];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_deep_obj_render_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Innermost level (walker depth == BB_SERIALIZE_MAX_DEPTH) omits the
    // "deep" field entirely (the walker's own depth guard fires before
    // begin_obj is ever called) -- only "marker" survives at the bottom.
    char expected[512];
    strcpy(expected, "{\"marker\":42}");
    for (int i = 0; i < BB_SERIALIZE_MAX_DEPTH; i++) {
        char wrapped[sizeof(expected) + 64];
        snprintf(wrapped, sizeof(wrapped), "{\"marker\":42,\"deep\":%s}", expected);
        strcpy(expected, wrapped);
    }

    TEST_ASSERT_EQUAL_STRING(expected, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

// 17b. arr-of-obj nested to BB_SERIALIZE_MAX_DEPTH -- the OOB-triggering
// shape: each walker level costs TWO json stack frames (the array's
// begin_arr plus, per element, the element's begin_obj). Real (non
// self-referential) nested instances, one per level, terminating in an
// empty "kids" array at the innermost level.
typedef struct deep_arr_render_lvl_s {
    int64_t            marker;
    bb_serialize_arr_t kids;
} deep_arr_render_lvl_t;

static const bb_serialize_field_t s_deep_arr_render_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_arr_render_lvl_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(deep_arr_render_lvl_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(deep_arr_render_lvl_t), .max_items = 1,
      .children = s_deep_arr_render_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_arr_render_desc = {
    .type_name = "deep_arr_render_lvl_t",
    .fields = s_deep_arr_render_fields,
    .n_fields = 2,
    .snap_size = sizeof(deep_arr_render_lvl_t),
};

void test_bb_serialize_json_render_deep_arr_of_obj(void)
{
    // Hand-link BB_SERIALIZE_MAX_DEPTH+1 real levels (walker depths 0..8):
    // levels[0] is the root snapshot, levels[MAX_DEPTH] is the innermost
    // (empty "kids", terminating the chain).
    deep_arr_render_lvl_t levels[BB_SERIALIZE_MAX_DEPTH + 1];
    for (int i = BB_SERIALIZE_MAX_DEPTH; i >= 0; i--) {
        levels[i].marker = i;
        if (i == BB_SERIALIZE_MAX_DEPTH) {
            levels[i].kids = (bb_serialize_arr_t){ .items = NULL, .count = 0 };
        } else {
            levels[i].kids = (bb_serialize_arr_t){ .items = &levels[i + 1], .count = 1 };
        }
    }

    char buf[1024];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_json_render(&s_deep_arr_render_desc, &levels[0], buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Build the expected string mechanically, independent of the writer:
    // innermost is {"marker":N,"kids":[]}, each outer level wraps one more
    // {"marker":N,"kids":[ ... ]}.
    char expected[1024];
    snprintf(expected, sizeof(expected), "{\"marker\":%d,\"kids\":[]}", BB_SERIALIZE_MAX_DEPTH);
    for (int i = BB_SERIALIZE_MAX_DEPTH - 1; i >= 0; i--) {
        char wrapped[sizeof(expected) + 64];
        snprintf(wrapped, sizeof(wrapped), "{\"marker\":%d,\"kids\":[%s]}", i, expected);
        strcpy(expected, wrapped);
    }

    TEST_ASSERT_EQUAL_STRING(expected, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

// 17c. push_level's runtime bounds guard -- the walker's own depth cap
// (BB_SERIALIZE_MAX_DEPTH) combined with the stack's exact worst-case
// sizing (2*BB_SERIALIZE_MAX_DEPTH+2) means a walker-driven render can
// reach the stack's very last slot (as in 17b above) but never exceed
// it -- so the guard's BB_ERR_NO_SPACE branch is unreachable via
// bb_serialize_json_render(). It IS reachable by manually over-driving
// the exported bb_serialize_json_emit() vtable (documented for manual
// control) past the stack's capacity -- exercised here to cover the
// guard branch and prove it degrades to a sticky error, never an OOB
// write (ASAN-clean).
void test_bb_serialize_json_push_level_guard_manual_overdrive(void)
{
    // Buffer sized generously so byte capacity is never the limiting
    // factor -- this must exercise the STACK-DEPTH guard specifically
    // (bb_json_push_level's own bounds check), not bb_json_put()'s
    // separate byte-capacity overflow.
    char buf[4096];
    bb_serialize_json_ctx_t ctx;
    bb_serialize_json_ctx_init(&ctx, buf, sizeof(buf) - 1);
    bb_serialize_emit_t emit = bb_serialize_json_emit(&ctx);

    // Stack capacity is 2*BB_SERIALIZE_MAX_DEPTH+2; push well past it.
    for (int i = 0; i < 4 * BB_SERIALIZE_MAX_DEPTH; i++) {
        emit.begin_obj(emit.ctx, "k");
    }

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, ctx.err);
    // Byte capacity was never exhausted -- confirms the error came from
    // the stack-depth guard, not bb_json_put()'s overflow check.
    TEST_ASSERT_TRUE(ctx.len < ctx.cap);

    // Further driving (including end_obj underflow) must stay a no-op /
    // safe -- no crash, no further state corruption.
    for (int i = 0; i < 4 * BB_SERIALIZE_MAX_DEPTH; i++) {
        emit.end_obj(emit.ctx);
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, ctx.err);
}

// 18. pop_level underflow guard -- an unbalanced end_obj/end_arr driven
// via the raw exported vtable (documented for manual control) must not
// wrap ctx->depth (a uint8_t) 0 -> 255. Confirms depth stays pinned at 0
// and a subsequent legitimate begin/end pair still works correctly.
void test_bb_serialize_json_pop_level_underflow_guard(void)
{
    char buf[64];
    bb_serialize_json_ctx_t ctx;
    bb_serialize_json_ctx_init(&ctx, buf, sizeof(buf) - 1);
    bb_serialize_emit_t emit = bb_serialize_json_emit(&ctx);

    // Unbalanced: end_obj with no matching begin_obj.
    emit.end_obj(emit.ctx);
    emit.end_obj(emit.ctx);
    TEST_ASSERT_EQUAL_UINT(0, ctx.depth);
    TEST_ASSERT_EQUAL(BB_OK, ctx.err);

    // A subsequent legitimate begin/end pair still works: depth pinned at
    // 0 means this re-enters at the root object level correctly.
    emit.begin_obj(emit.ctx, "k");
    emit.end_obj(emit.ctx);
    TEST_ASSERT_EQUAL_UINT(0, ctx.depth);
    TEST_ASSERT_EQUAL(BB_OK, ctx.err);
}
