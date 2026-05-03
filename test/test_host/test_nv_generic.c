#include "unity.h"
#include "bb_nv.h"
#include <string.h>

// bb_nv_set_u8 tests
void test_nv_set_u8_null_ns(void)
{
    bb_err_t err = bb_nv_set_u8(NULL, "key", 42);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_u8_null_key(void)
{
    bb_err_t err = bb_nv_set_u8("ns", NULL, 42);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_u8_valid(void)
{
    bb_err_t err = bb_nv_set_u8("ns", "key", 42);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// bb_nv_set_u32 tests
void test_nv_set_u32_null_ns(void)
{
    bb_err_t err = bb_nv_set_u32(NULL, "key", 0x12345678);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_u32_null_key(void)
{
    bb_err_t err = bb_nv_set_u32("ns", NULL, 0x12345678);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_u32_valid(void)
{
    bb_err_t err = bb_nv_set_u32("ns", "key", 0x12345678);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// bb_nv_set_str tests
void test_nv_set_str_null_ns(void)
{
    bb_err_t err = bb_nv_set_str(NULL, "key", "value");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_str_null_key(void)
{
    bb_err_t err = bb_nv_set_str("ns", NULL, "value");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_str_null_value(void)
{
    bb_err_t err = bb_nv_set_str("ns", "key", NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_str_valid(void)
{
    bb_err_t err = bb_nv_set_str("ns", "key", "test_value");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// bb_nv_get_u8 tests
void test_nv_get_u8_null_ns(void)
{
    uint8_t val = 0;
    bb_err_t err = bb_nv_get_u8(NULL, "key", &val, 99);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u8_null_key(void)
{
    uint8_t val = 0;
    bb_err_t err = bb_nv_get_u8("ns", NULL, &val, 99);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u8_null_out(void)
{
    bb_err_t err = bb_nv_get_u8("ns", "key", NULL, 99);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u8_returns_fallback(void)
{
    uint8_t val = 0;
    bb_err_t err = bb_nv_get_u8("ns", "key", &val, 99);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(99, val);
}

// bb_nv_get_u32 tests
void test_nv_get_u32_null_ns(void)
{
    uint32_t val = 0;
    bb_err_t err = bb_nv_get_u32(NULL, "key", &val, 0xDEADBEEF);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u32_null_key(void)
{
    uint32_t val = 0;
    bb_err_t err = bb_nv_get_u32("ns", NULL, &val, 0xDEADBEEF);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u32_null_out(void)
{
    bb_err_t err = bb_nv_get_u32("ns", "key", NULL, 0xDEADBEEF);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u32_returns_fallback(void)
{
    uint32_t val = 0;
    bb_err_t err = bb_nv_get_u32("ns", "key", &val, 0xDEADBEEF);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0xDEADBEEF, val);
}

// bb_nv_get_str tests
void test_nv_get_str_null_ns(void)
{
    char buf[16];
    bb_err_t err = bb_nv_get_str(NULL, "key", buf, sizeof(buf), "fallback");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_str_null_key(void)
{
    char buf[16];
    bb_err_t err = bb_nv_get_str("ns", NULL, buf, sizeof(buf), "fallback");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_str_null_buf(void)
{
    bb_err_t err = bb_nv_get_str("ns", "key", NULL, 16, "fallback");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_str_zero_len(void)
{
    char buf[16];
    bb_err_t err = bb_nv_get_str("ns", "key", buf, 0, "fallback");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_str_returns_fallback(void)
{
    char buf[16];
    bb_err_t err = bb_nv_get_str("ns", "key", buf, sizeof(buf), "fallback");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("fallback", buf);
}

void test_nv_get_str_returns_fallback_null(void)
{
    char buf[16];
    bb_err_t err = bb_nv_get_str("ns", "key", buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_nv_get_str_truncates_fallback(void)
{
    char buf[8];
    bb_err_t err = bb_nv_get_str("ns", "key", buf, sizeof(buf), "very_long_fallback");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("very_lo", buf);
}

// bb_nv_erase tests
void test_nv_erase_null_ns(void)
{
    bb_err_t err = bb_nv_erase(NULL, "key");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_erase_null_key(void)
{
    bb_err_t err = bb_nv_erase("ns", NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_erase_valid(void)
{
    bb_err_t err = bb_nv_erase("ns", "key");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// bb_nv_set_u16 tests
void test_nv_set_u16_null_ns(void)
{
    bb_err_t err = bb_nv_set_u16(NULL, "key", 42);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_u16_null_key(void)
{
    bb_err_t err = bb_nv_set_u16("ns", NULL, 42);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_nv_set_u16_valid(void)
{
    bb_err_t err = bb_nv_set_u16("ns", "key", 0xBEEF);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

// bb_nv_get_u16 tests
void test_nv_get_u16_null_ns(void)
{
    uint16_t val = 0;
    bb_err_t err = bb_nv_get_u16(NULL, "key", &val, 0xBEEF);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u16_null_key(void)
{
    uint16_t val = 0;
    bb_err_t err = bb_nv_get_u16("ns", NULL, &val, 0xBEEF);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u16_null_out(void)
{
    bb_err_t err = bb_nv_get_u16("ns", "key", NULL, 0xBEEF);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_nv_get_u16_returns_fallback(void)
{
    uint16_t val = 0;
    bb_err_t err = bb_nv_get_u16("ns", "key", &val, 0xBEEF);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_UINT16(0xBEEF, val);
}

// -------- batched setter tests --------

void test_nv_batch_begin_null_batch(void)
{
    bb_err_t err = bb_nv_batch_begin(NULL, "ns");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_batch_begin_null_ns(void)
{
    bb_nv_batch_t batch;
    bb_err_t err = bb_nv_batch_begin(&batch, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_batch_begin_valid(void)
{
    bb_nv_batch_t batch;
    bb_err_t err = bb_nv_batch_begin(&batch, "ns");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    bb_nv_batch_commit(&batch);
}

void test_nv_batch_set_u32_before_begin(void)
{
    bb_nv_batch_t batch = {0};
    bb_err_t err = bb_nv_batch_set_u32(&batch, "k", 1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
}

void test_nv_batch_set_u32_null_key(void)
{
    bb_nv_batch_t batch;
    bb_nv_batch_begin(&batch, "ns");
    bb_err_t err = bb_nv_batch_set_u32(&batch, NULL, 1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
    bb_nv_batch_commit(&batch);
}

void test_nv_batch_set_u32_valid(void)
{
    bb_nv_batch_t batch;
    bb_nv_batch_begin(&batch, "ns");
    bb_err_t err = bb_nv_batch_set_u32(&batch, "k", 0xFEEDFACE);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    bb_nv_batch_commit(&batch);
}

void test_nv_batch_set_str_null_value(void)
{
    bb_nv_batch_t batch;
    bb_nv_batch_begin(&batch, "ns");
    bb_err_t err = bb_nv_batch_set_str(&batch, "k", NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
    bb_nv_batch_commit(&batch);
}

void test_nv_batch_commit_null(void)
{
    bb_err_t err = bb_nv_batch_commit(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_nv_batch_set_after_commit_rejected(void)
{
    bb_nv_batch_t batch;
    bb_nv_batch_begin(&batch, "ns");
    bb_nv_batch_commit(&batch);
    bb_err_t err = bb_nv_batch_set_u32(&batch, "k", 1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
}

void test_nv_batch_three_u32_writes_succeed(void)
{
    /* Mirrors the TM mining_stats_save_lifetime call shape. */
    bb_nv_batch_t batch;
    bb_err_t err = bb_nv_batch_begin(&batch, "taipanminer");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = bb_nv_batch_set_u32(&batch, "lt_shares",    100);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = bb_nv_batch_set_u32(&batch, "lt_hashes_lo", 0xCAFEBABE);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = bb_nv_batch_set_u32(&batch, "lt_hashes_hi", 0xDEADBEEF);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = bb_nv_batch_commit(&batch);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}
