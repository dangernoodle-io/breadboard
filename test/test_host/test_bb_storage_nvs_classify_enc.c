#include "unity.h"
#include "bb_storage_nvs_classify_enc.h"

// Pure bb_storage_enc_t -> bb_storage_nvs_kind_t mapping, extracted from
// nvs_vt_get_typed/nvs_vt_set_typed's dispatch switch so Coveralls sees
// every enc branch without requiring NVS. See
// components/bb_storage_nvs/src/bb_storage_nvs_classify_enc.h.

void test_bb_storage_nvs_classify_enc_blob(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_KIND_BLOB, bb_storage_nvs_classify_enc(BB_STORAGE_ENC_BLOB));
}

void test_bb_storage_nvs_classify_enc_str(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_KIND_STR, bb_storage_nvs_classify_enc(BB_STORAGE_ENC_STR));
}

void test_bb_storage_nvs_classify_enc_u8(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_KIND_U8, bb_storage_nvs_classify_enc(BB_STORAGE_ENC_U8));
}

void test_bb_storage_nvs_classify_enc_u16(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_KIND_U16, bb_storage_nvs_classify_enc(BB_STORAGE_ENC_U16));
}

void test_bb_storage_nvs_classify_enc_u32(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_KIND_U32, bb_storage_nvs_classify_enc(BB_STORAGE_ENC_U32));
}

void test_bb_storage_nvs_classify_enc_i32(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_KIND_I32, bb_storage_nvs_classify_enc(BB_STORAGE_ENC_I32));
}

void test_bb_storage_nvs_classify_enc_unknown_defaults_to_blob(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_KIND_BLOB, bb_storage_nvs_classify_enc((bb_storage_enc_t)99));
}
