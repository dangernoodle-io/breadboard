#include "unity.h"
#include "bb_config.h"
#include "bb_storage.h"
#include "bb_storage_ram.h"
#include "bb_core.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/
static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_ram_test_reset();
    bb_storage_ram_register();
}

#define ADDR(k) { .backend = "ram", .ns_or_dir = NULL, .key = (k) }

static const bb_config_field_t F_BOOL_NODEF = {
    .id = "test.bool_nodef", .type = BB_CONFIG_BOOL, .addr = ADDR("bool_nodef"),
};
static const bb_config_field_t F_BOOL_DEF = {
    .id = "test.bool_def", .type = BB_CONFIG_BOOL, .addr = ADDR("bool_def"),
    .has_default = true, .def = { .b = true },
};

static const bb_config_field_t F_U8_NODEF = {
    .id = "test.u8_nodef", .type = BB_CONFIG_U8, .addr = ADDR("u8_nodef"),
};
static const bb_config_field_t F_U8_DEF = {
    .id = "test.u8_def", .type = BB_CONFIG_U8, .addr = ADDR("u8_def"),
    .has_default = true, .def = { .u8 = 42 },
};

static const bb_config_field_t F_U16_NODEF = {
    .id = "test.u16_nodef", .type = BB_CONFIG_U16, .addr = ADDR("u16_nodef"),
};
static const bb_config_field_t F_U16_DEF = {
    .id = "test.u16_def", .type = BB_CONFIG_U16, .addr = ADDR("u16_def"),
    .has_default = true, .def = { .u16 = 4242 },
};

static const bb_config_field_t F_U32_NODEF = {
    .id = "test.u32_nodef", .type = BB_CONFIG_U32, .addr = ADDR("u32_nodef"),
};
static const bb_config_field_t F_U32_DEF = {
    .id = "test.u32_def", .type = BB_CONFIG_U32, .addr = ADDR("u32_def"),
    .has_default = true, .def = { .u32 = 424242u },
};

static const bb_config_field_t F_I32_NODEF = {
    .id = "test.i32_nodef", .type = BB_CONFIG_I32, .addr = ADDR("i32_nodef"),
};
static const bb_config_field_t F_I32_DEF = {
    .id = "test.i32_def", .type = BB_CONFIG_I32, .addr = ADDR("i32_def"),
    .has_default = true, .def = { .i32 = -4242 },
};

static const bb_config_field_t F_STR_NODEF = {
    .id = "test.str_nodef", .type = BB_CONFIG_STR, .addr = ADDR("str_nodef"), .max_len = 16,
};
static const bb_config_field_t F_STR_DEF = {
    .id = "test.str_def", .type = BB_CONFIG_STR, .addr = ADDR("str_def"), .max_len = 16,
    .has_default = true, .def = { .str = "defval" },
};

static const bb_config_field_t F_BLOB = {
    .id = "test.blob", .type = BB_CONFIG_BLOB, .addr = ADDR("blob"), .max_len = 16,
};

/* ---------------------------------------------------------------------------
 * bool
 * ---------------------------------------------------------------------------*/
void test_bb_config_bool_set_then_get_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_bool(&F_BOOL_NODEF, true));
    bool out = false;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_bool(&F_BOOL_NODEF, &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_config_bool_missing_no_default_returns_not_found(void)
{
    reset_all();
    bool out = false;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_bool(&F_BOOL_NODEF, &out));
}

void test_bb_config_bool_missing_with_default_resolves_default(void)
{
    reset_all();
    bool out = false;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_bool(&F_BOOL_DEF, &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_config_bool_wrong_type_get_returns_invalid_arg(void)
{
    reset_all();
    uint8_t out8 = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u8(&F_BOOL_NODEF, &out8));
}

void test_bb_config_bool_wrong_type_set_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_u8(&F_BOOL_NODEF, 1));
}

/* ---------------------------------------------------------------------------
 * u8
 * ---------------------------------------------------------------------------*/
void test_bb_config_u8_set_then_get_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&F_U8_NODEF, 200));
    uint8_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&F_U8_NODEF, &out));
    TEST_ASSERT_EQUAL_UINT8(200, out);
}

void test_bb_config_u8_missing_no_default_returns_not_found(void)
{
    reset_all();
    uint8_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_u8(&F_U8_NODEF, &out));
}

void test_bb_config_u8_missing_with_default_resolves_default(void)
{
    reset_all();
    uint8_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&F_U8_DEF, &out));
    TEST_ASSERT_EQUAL_UINT8(42, out);
}

void test_bb_config_u8_wrong_type_get_returns_invalid_arg(void)
{
    reset_all();
    bool outb = false;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_bool(&F_U8_NODEF, &outb));
}

void test_bb_config_u8_wrong_type_set_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_bool(&F_U8_NODEF, true));
}

/* ---------------------------------------------------------------------------
 * u16
 * ---------------------------------------------------------------------------*/
void test_bb_config_u16_set_then_get_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u16(&F_U16_NODEF, 60000));
    uint16_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u16(&F_U16_NODEF, &out));
    TEST_ASSERT_EQUAL_UINT16(60000, out);
}

void test_bb_config_u16_missing_no_default_returns_not_found(void)
{
    reset_all();
    uint16_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_u16(&F_U16_NODEF, &out));
}

void test_bb_config_u16_missing_with_default_resolves_default(void)
{
    reset_all();
    uint16_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u16(&F_U16_DEF, &out));
    TEST_ASSERT_EQUAL_UINT16(4242, out);
}

void test_bb_config_u16_wrong_type_get_returns_invalid_arg(void)
{
    reset_all();
    uint8_t out8 = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u8(&F_U16_NODEF, &out8));
}

void test_bb_config_u16_wrong_type_set_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_u8(&F_U16_NODEF, 1));
}

/* ---------------------------------------------------------------------------
 * u32
 * ---------------------------------------------------------------------------*/
void test_bb_config_u32_set_then_get_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u32(&F_U32_NODEF, 4000000000u));
    uint32_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u32(&F_U32_NODEF, &out));
    TEST_ASSERT_EQUAL_UINT32(4000000000u, out);
}

void test_bb_config_u32_missing_no_default_returns_not_found(void)
{
    reset_all();
    uint32_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_u32(&F_U32_NODEF, &out));
}

void test_bb_config_u32_missing_with_default_resolves_default(void)
{
    reset_all();
    uint32_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u32(&F_U32_DEF, &out));
    TEST_ASSERT_EQUAL_UINT32(424242u, out);
}

void test_bb_config_u32_wrong_type_get_returns_invalid_arg(void)
{
    reset_all();
    uint8_t out8 = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u8(&F_U32_NODEF, &out8));
}

void test_bb_config_u32_wrong_type_set_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_u8(&F_U32_NODEF, 1));
}

/* ---------------------------------------------------------------------------
 * i32
 * ---------------------------------------------------------------------------*/
void test_bb_config_i32_set_then_get_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_i32(&F_I32_NODEF, -123456));
    int32_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_i32(&F_I32_NODEF, &out));
    TEST_ASSERT_EQUAL_INT32(-123456, out);
}

void test_bb_config_i32_missing_no_default_returns_not_found(void)
{
    reset_all();
    int32_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_i32(&F_I32_NODEF, &out));
}

void test_bb_config_i32_missing_with_default_resolves_default(void)
{
    reset_all();
    int32_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_i32(&F_I32_DEF, &out));
    TEST_ASSERT_EQUAL_INT32(-4242, out);
}

void test_bb_config_i32_wrong_type_get_returns_invalid_arg(void)
{
    reset_all();
    uint8_t out8 = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u8(&F_I32_NODEF, &out8));
}

void test_bb_config_i32_wrong_type_set_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_u8(&F_I32_NODEF, 1));
}

/* ---------------------------------------------------------------------------
 * str
 * ---------------------------------------------------------------------------*/
void test_bb_config_str_set_then_get_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&F_STR_NODEF, "hello"));
    char buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&F_STR_NODEF, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(5, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("hello", buf, out_len);
}

void test_bb_config_str_missing_no_default_returns_not_found(void)
{
    reset_all();
    char buf[16];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_str(&F_STR_NODEF, buf, sizeof(buf), &out_len));
}

void test_bb_config_str_missing_with_default_resolves_default(void)
{
    reset_all();
    char buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&F_STR_DEF, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(6, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("defval", buf, out_len);
}

void test_bb_config_str_wrong_type_get_returns_invalid_arg(void)
{
    reset_all();
    uint8_t out8 = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u8(&F_STR_NODEF, &out8));
}

void test_bb_config_str_wrong_type_set_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_u8(&F_STR_NODEF, 1));
}

void test_bb_config_str_overflow_rejected(void)
{
    reset_all();
    /* max_len=16 -- 16-char value has strlen == max_len, must be rejected */
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_str(&F_STR_NODEF, "0123456789012345"));
}

void test_bb_config_str_cap_zero_probes_length(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&F_STR_NODEF, "abcde"));
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&F_STR_NODEF, NULL, 0, &out_len));
    TEST_ASSERT_EQUAL(5, out_len);
}

void test_bb_config_str_truncation_reports_full_len(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&F_STR_NODEF, "0123456789"));
    char buf[4] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&F_STR_NODEF, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(10, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("0123", buf, sizeof(buf));
}

/* ---------------------------------------------------------------------------
 * blob
 * ---------------------------------------------------------------------------*/
void test_bb_config_blob_set_then_get_round_trip(void)
{
    reset_all();
    const uint8_t v[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_blob(&F_BLOB, v, sizeof(v)));
    uint8_t buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_blob(&F_BLOB, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(sizeof(v), out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(v, buf, sizeof(v));
}

void test_bb_config_blob_missing_returns_not_found(void)
{
    reset_all();
    uint8_t buf[16];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_blob(&F_BLOB, buf, sizeof(buf), &out_len));
}

void test_bb_config_blob_wrong_type_get_returns_invalid_arg(void)
{
    reset_all();
    uint8_t out8 = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u8(&F_BLOB, &out8));
}

void test_bb_config_blob_wrong_type_set_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_u8(&F_BLOB, 1));
}

void test_bb_config_blob_over_max_len_rejected(void)
{
    reset_all();
    uint8_t big[17];
    memset(big, 'a', sizeof(big));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_blob(&F_BLOB, big, sizeof(big)));
}

void test_bb_config_blob_cap_zero_probes_length(void)
{
    reset_all();
    const uint8_t v[3] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_blob(&F_BLOB, v, sizeof(v)));
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_blob(&F_BLOB, NULL, 0, &out_len));
    TEST_ASSERT_EQUAL(3, out_len);
}

/* ---------------------------------------------------------------------------
 * NULL-arg branches (shared scalar_get/scalar_set + str/blob validation)
 * ---------------------------------------------------------------------------*/
void test_bb_config_get_bool_null_out_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_bool(&F_BOOL_NODEF, NULL));
}

void test_bb_config_get_u16_null_out_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u16(&F_U16_NODEF, NULL));
}

void test_bb_config_get_u32_null_out_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u32(&F_U32_NODEF, NULL));
}

void test_bb_config_get_i32_null_out_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_i32(&F_I32_NODEF, NULL));
}

void test_bb_config_get_u8_null_field_returns_invalid_arg(void)
{
    reset_all();
    uint8_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_u8(NULL, &out));
}

void test_bb_config_set_u8_null_field_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_u8(NULL, 1));
}

void test_bb_config_get_str_null_field_returns_invalid_arg(void)
{
    reset_all();
    char buf[16];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_str(NULL, buf, sizeof(buf), &out_len));
}

void test_bb_config_get_str_null_out_len_returns_invalid_arg(void)
{
    reset_all();
    char buf[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_str(&F_STR_NODEF, buf, sizeof(buf), NULL));
}

void test_bb_config_get_str_null_buf_with_nonzero_cap_returns_invalid_arg(void)
{
    reset_all();
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_str(&F_STR_NODEF, NULL, 4, &out_len));
}

void test_bb_config_set_str_null_field_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_str(NULL, "x"));
}

void test_bb_config_set_str_null_value_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_str(&F_STR_NODEF, NULL));
}

void test_bb_config_get_blob_null_field_returns_invalid_arg(void)
{
    reset_all();
    uint8_t buf[16];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_blob(NULL, buf, sizeof(buf), &out_len));
}

void test_bb_config_get_blob_null_out_len_returns_invalid_arg(void)
{
    reset_all();
    uint8_t buf[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_blob(&F_BLOB, buf, sizeof(buf), NULL));
}

void test_bb_config_get_blob_null_buf_with_nonzero_cap_returns_invalid_arg(void)
{
    reset_all();
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_get_blob(&F_BLOB, NULL, 4, &out_len));
}

void test_bb_config_set_blob_null_field_returns_invalid_arg(void)
{
    reset_all();
    uint8_t v[1] = { 1 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_blob(NULL, v, sizeof(v)));
}

void test_bb_config_set_blob_null_value_with_nonzero_len_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_set_blob(&F_BLOB, NULL, 4));
}

void test_bb_config_set_blob_null_value_zero_len_is_ok(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_blob(&F_BLOB, NULL, 0));
    TEST_ASSERT_TRUE(bb_config_exists(&F_BLOB));
}

/* ---------------------------------------------------------------------------
 * scalar width-mismatch guard (short/foreign/corrupt stored value)
 * ---------------------------------------------------------------------------*/
void test_bb_config_u32_short_stored_value_returns_invalid_state(void)
{
    reset_all();
    /* Seed a 1-byte value directly under a u32 field's addr -- shorter than
     * the accessor's fixed 4-byte width. */
    const uint8_t one_byte = 0xAB;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&F_U32_NODEF.addr, &one_byte, sizeof(one_byte)));
    uint32_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_config_get_u32(&F_U32_NODEF, &out));
}

/* ---------------------------------------------------------------------------
 * str default overflowing its own max_len (static misconfiguration)
 * ---------------------------------------------------------------------------*/
static const bb_config_field_t F_STR_DEF_TOO_LONG = {
    .id = "test.str_def_too_long", .type = BB_CONFIG_STR, .addr = ADDR("str_def_too_long"), .max_len = 4,
    .has_default = true, .def = { .str = "this default is way too long" },
};

void test_bb_config_str_default_longer_than_max_len_returns_invalid_arg(void)
{
    reset_all();
    char buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_config_get_str(&F_STR_DEF_TOO_LONG, buf, sizeof(buf), &out_len));
}

/* ---------------------------------------------------------------------------
 * erase / exists
 * ---------------------------------------------------------------------------*/
void test_bb_config_erase_removes_value(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&F_U8_NODEF, 1));
    TEST_ASSERT_TRUE(bb_config_exists(&F_U8_NODEF));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_erase(&F_U8_NODEF));
    TEST_ASSERT_FALSE(bb_config_exists(&F_U8_NODEF));
}

void test_bb_config_erase_missing_is_idempotent(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_erase(&F_U8_NODEF));
}

void test_bb_config_erase_null_field_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_erase(NULL));
}

void test_bb_config_exists_false_before_set_true_after(void)
{
    reset_all();
    TEST_ASSERT_FALSE(bb_config_exists(&F_U8_NODEF));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&F_U8_NODEF, 1));
    TEST_ASSERT_TRUE(bb_config_exists(&F_U8_NODEF));
}

void test_bb_config_exists_false_for_null_field(void)
{
    reset_all();
    TEST_ASSERT_FALSE(bb_config_exists(NULL));
}
