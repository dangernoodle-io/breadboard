// Tests for bb_partition_serialize -- walks bb_partition_row_fields against
// one bb_partition_row_wire_t (widened from the host mock's first entry,
// bb_partition_list()'s "nvs" row) through a recording bb_serialize_emit_t
// fake, mirroring test_bb_serialize.c's recording-mock idiom.
#include "unity.h"
#include "bb_partition_serialize.h"

#include <string.h>

typedef enum {
    OP_U64,
    OP_BOOL,
    OP_STR,
} rec_op_t;

typedef struct {
    rec_op_t    op;
    const char *key;
    uint64_t    u64;
    bool        b;
    char        str_val[32];
    size_t      str_len;
} rec_t;

#define REC_MAX 8

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

static void mock_begin_obj(void *ctx, const char *key) { (void)ctx; (void)key; TEST_FAIL_MESSAGE("unexpected begin_obj"); }
static void mock_end_obj(void *ctx) { (void)ctx; TEST_FAIL_MESSAGE("unexpected end_obj"); }
static void mock_begin_arr(void *ctx, const char *key) { (void)ctx; (void)key; TEST_FAIL_MESSAGE("unexpected begin_arr"); }
static void mock_end_arr(void *ctx) { (void)ctx; TEST_FAIL_MESSAGE("unexpected end_arr"); }
static void mock_emit_i64(void *ctx, const char *key, int64_t v) { (void)ctx; (void)key; (void)v; TEST_FAIL_MESSAGE("unexpected emit_i64"); }
static void mock_emit_f64(void *ctx, const char *key, double v) { (void)ctx; (void)key; (void)v; TEST_FAIL_MESSAGE("unexpected emit_f64"); }
static void mock_emit_null(void *ctx, const char *key) { (void)ctx; (void)key; TEST_FAIL_MESSAGE("unexpected emit_null"); }

static void mock_emit_u64(void *ctx, const char *key, uint64_t v)
{
    (void)ctx;
    rec_push(OP_U64, key)->u64 = v;
}

static void mock_emit_bool(void *ctx, const char *key, bool v)
{
    (void)ctx;
    rec_push(OP_BOOL, key)->b = v;
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

static const bb_serialize_desc_t s_row_desc = {
    .type_name = "bb_partition_row_wire_t",
    .fields    = bb_partition_row_fields,
    .n_fields  = 0,  // set at test time from bb_partition_row_n_fields
    .snap_size = sizeof(bb_partition_row_wire_t),
};

// 1: n_fields matches the 7-field table the header documents.
void test_bb_partition_serialize_n_fields(void)
{
    TEST_ASSERT_EQUAL_UINT16(7, bb_partition_row_n_fields);
}

// 2: widening helper NULL-guards both args (no crash, no write).
void test_bb_partition_serialize_wire_from_info_null_guards(void)
{
    bb_partition_row_wire_t wire;
    memset(&wire, 0xAA, sizeof(wire));

    // Keep a reference copy of the 0xAA-filled bytes before calling the function
    uint8_t reference[sizeof(bb_partition_row_wire_t)];
    memcpy(reference, &wire, sizeof(wire));

    bb_partition_info_t info;
    memset(&info, 0, sizeof(info));

    bb_partition_row_wire_from_info(NULL, &info);
    bb_partition_row_wire_from_info(&wire, NULL);

    // Verify the struct is completely untouched -- still the 0xAA sentinel pattern
    TEST_ASSERT_EQUAL_UINT8_ARRAY(reference, (uint8_t *)&wire, sizeof(bb_partition_row_wire_t));
}

// 3: walk the "nvs" row (bb_partition_list() host mock index 0) through the
// recording emit fake -- each of the 7 fields emits exactly once, in table
// order, with the widened value.
void test_bb_partition_serialize_walk_nvs_row(void)
{
    bb_partition_info_t buf[8];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_list(buf, 8, &count));
    TEST_ASSERT_TRUE(count >= 1);
    TEST_ASSERT_EQUAL_STRING("nvs", buf[0].subtype);

    bb_partition_row_wire_t wire;
    bb_partition_row_wire_from_info(&wire, &buf[0]);

    rec_reset();
    bb_serialize_desc_t desc = s_row_desc;
    desc.n_fields = bb_partition_row_n_fields;
    bb_serialize_walk(&desc, &wire, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(7, s_rec_n);

    TEST_ASSERT_EQUAL(OP_STR, s_rec[0].op);
    TEST_ASSERT_EQUAL_STRING("label", s_rec[0].key);
    TEST_ASSERT_EQUAL_STRING("nvs", s_rec[0].str_val);

    TEST_ASSERT_EQUAL(OP_STR, s_rec[1].op);
    TEST_ASSERT_EQUAL_STRING("type", s_rec[1].key);
    TEST_ASSERT_EQUAL_STRING("data", s_rec[1].str_val);

    TEST_ASSERT_EQUAL(OP_STR, s_rec[2].op);
    TEST_ASSERT_EQUAL_STRING("subtype", s_rec[2].key);
    TEST_ASSERT_EQUAL_STRING("nvs", s_rec[2].str_val);

    TEST_ASSERT_EQUAL(OP_U64, s_rec[3].op);
    TEST_ASSERT_EQUAL_STRING("offset", s_rec[3].key);
    TEST_ASSERT_EQUAL_UINT64(0x009000, s_rec[3].u64);

    TEST_ASSERT_EQUAL(OP_U64, s_rec[4].op);
    TEST_ASSERT_EQUAL_STRING("size", s_rec[4].key);
    TEST_ASSERT_EQUAL_UINT64(0x006000, s_rec[4].u64);

    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[5].op);
    TEST_ASSERT_EQUAL_STRING("running", s_rec[5].key);
    TEST_ASSERT_FALSE(s_rec[5].b);

    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[6].op);
    TEST_ASSERT_EQUAL_STRING("next_ota", s_rec[6].key);
    TEST_ASSERT_FALSE(s_rec[6].b);
}

// 4: the running/next_ota "ota_0" row (index 2) round-trips true booleans
// and a 32-bit offset/size pair correctly widened -- proves the U64 widening
// doesn't corrupt values that use the top bits of a real ESP32 4MB-class
// flash offset (0x1b0000 spans multiple bytes, not just the low byte).
void test_bb_partition_serialize_walk_ota0_row(void)
{
    bb_partition_info_t buf[8];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_list(buf, 8, &count));
    TEST_ASSERT_TRUE(count >= 3);
    TEST_ASSERT_EQUAL_STRING("ota_0", buf[2].subtype);

    bb_partition_row_wire_t wire;
    bb_partition_row_wire_from_info(&wire, &buf[2]);

    rec_reset();
    bb_serialize_desc_t desc = s_row_desc;
    desc.n_fields = bb_partition_row_n_fields;
    bb_serialize_walk(&desc, &wire, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(7, s_rec_n);
    TEST_ASSERT_EQUAL_UINT64(0x020000, s_rec[3].u64);
    TEST_ASSERT_EQUAL_UINT64(0x1b0000, s_rec[4].u64);
    TEST_ASSERT_TRUE(s_rec[5].b);
    TEST_ASSERT_FALSE(s_rec[6].b);
}
