#include "unity.h"
#include "bb_config.h"
#include "bb_config_test.h"
#include "bb_storage.h"

// Pure bb_config_type_t -> bb_storage_enc_t mapping, host-tested directly via
// the BB_CONFIG_TESTING hook (bb_config_test.h). See bb_config.c's
// cfg_type_to_enc.

void test_cfg_type_to_enc_bool_maps_to_u8(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U8, bb_config_cfg_type_to_enc_for_test(BB_CONFIG_BOOL));
}

void test_cfg_type_to_enc_u8_maps_to_u8(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U8, bb_config_cfg_type_to_enc_for_test(BB_CONFIG_U8));
}

void test_cfg_type_to_enc_u16_maps_to_u16(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U16, bb_config_cfg_type_to_enc_for_test(BB_CONFIG_U16));
}

void test_cfg_type_to_enc_u32_maps_to_u32(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U32, bb_config_cfg_type_to_enc_for_test(BB_CONFIG_U32));
}

void test_cfg_type_to_enc_i32_maps_to_i32(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_I32, bb_config_cfg_type_to_enc_for_test(BB_CONFIG_I32));
}

void test_cfg_type_to_enc_str_maps_to_str(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_STR, bb_config_cfg_type_to_enc_for_test(BB_CONFIG_STR));
}

void test_cfg_type_to_enc_blob_maps_to_blob(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_BLOB, bb_config_cfg_type_to_enc_for_test(BB_CONFIG_BLOB));
}
