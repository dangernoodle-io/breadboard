#include "unity.h"
#include "bb_mdns_cache.h"

static const bb_mdns_txt_field_t s_fields[] = {
    { .txt_key = "board", .dest_offset = 0, .dest_len = 16 },
};

void test_bb_mdns_cache_validate_config_txt_fields_without_entry_size_rejected(void)
{
    bb_err_t err = bb_mdns_cache_validate_config(0, s_fields, 1, 192);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_cache_validate_config_entry_size_exceeds_max_rejected(void)
{
    bb_err_t err = bb_mdns_cache_validate_config(256, NULL, 0, 192);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_cache_validate_config_effective_default_size_exceeds_max_rejected(void)
{
    // entry_size == 0 falls back to sizeof(bb_mdns_cache_entry_t); an
    // entry_max smaller than that identity-only shape must still reject.
    bb_err_t err = bb_mdns_cache_validate_config(0, NULL, 0, 1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_cache_validate_config_pass_case_accepted(void)
{
    bb_err_t err = bb_mdns_cache_validate_config(64, s_fields, 1, 192);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

void test_bb_mdns_cache_validate_config_no_descriptor_default_size_accepted(void)
{
    bb_err_t err = bb_mdns_cache_validate_config(0, NULL, 0, 192);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

void test_bb_mdns_cache_validate_config_txt_fields_zero_count_default_size_accepted(void)
{
    // txt_fields non-NULL but txt_count == 0 -- not a descriptor (nothing to
    // apply), so entry_size == 0 is fine here.
    bb_err_t err = bb_mdns_cache_validate_config(0, s_fields, 0, 192);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}
