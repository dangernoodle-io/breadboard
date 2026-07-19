#include "unity.h"
#include "bb_storage_nvs_classify_nvs_type.h"

// Pure ESP-IDF-nvs_type_t-raw-code -> bb_storage_enc_t mapping, extracted so
// PR6's list_entries enumeration path is host-testable without nvs.h. See
// components/bb_storage_nvs/src/bb_storage_nvs_classify_nvs_type.h.

void test_bb_storage_nvs_classify_nvs_type_str(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_STR,
                       bb_storage_nvs_classify_nvs_type(BB_STORAGE_NVS_RAW_TYPE_STR));
}

void test_bb_storage_nvs_classify_nvs_type_u8(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U8,
                       bb_storage_nvs_classify_nvs_type(BB_STORAGE_NVS_RAW_TYPE_U8));
}

void test_bb_storage_nvs_classify_nvs_type_u16(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U16,
                       bb_storage_nvs_classify_nvs_type(BB_STORAGE_NVS_RAW_TYPE_U16));
}

void test_bb_storage_nvs_classify_nvs_type_u32(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U32,
                       bb_storage_nvs_classify_nvs_type(BB_STORAGE_NVS_RAW_TYPE_U32));
}

void test_bb_storage_nvs_classify_nvs_type_i32(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_I32,
                       bb_storage_nvs_classify_nvs_type(BB_STORAGE_NVS_RAW_TYPE_I32));
}

void test_bb_storage_nvs_classify_nvs_type_blob(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_BLOB,
                       bb_storage_nvs_classify_nvs_type(BB_STORAGE_NVS_RAW_TYPE_BLOB));
}

// Unrepresentable NVS types (no bb_storage_enc_t equivalent) fall back to
// BLOB rather than erroring -- PR6's documented contract.
void test_bb_storage_nvs_classify_nvs_type_i8_falls_back_to_blob(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_BLOB,
                       bb_storage_nvs_classify_nvs_type(BB_STORAGE_NVS_RAW_TYPE_I8));
}

void test_bb_storage_nvs_classify_nvs_type_u64_falls_back_to_blob(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_BLOB,
                       bb_storage_nvs_classify_nvs_type(BB_STORAGE_NVS_RAW_TYPE_U64));
}

void test_bb_storage_nvs_classify_nvs_type_unknown_defaults_to_blob(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_BLOB, bb_storage_nvs_classify_nvs_type(0x7777));
}
