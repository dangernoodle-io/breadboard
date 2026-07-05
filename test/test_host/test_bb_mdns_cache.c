#include "unity.h"
#include "bb_mdns_cache.h"

#include <string.h>

// ---------------------------------------------------------------------------
// bb_mdns_cache_build_key
// ---------------------------------------------------------------------------

void test_bb_mdns_cache_build_key_default_prefix(void)
{
    char out[BB_MDNS_CACHE_KEY_MAX];
    bb_err_t err = bb_mdns_cache_build_key(NULL, "TaipanMiner-abc123", out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("miner.TaipanMiner-abc123", out);
}

void test_bb_mdns_cache_build_key_empty_prefix_defaults(void)
{
    char out[BB_MDNS_CACHE_KEY_MAX];
    bb_err_t err = bb_mdns_cache_build_key("", "TaipanMiner-abc123", out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("miner.TaipanMiner-abc123", out);
}

void test_bb_mdns_cache_build_key_custom_prefix(void)
{
    char out[BB_MDNS_CACHE_KEY_MAX];
    bb_err_t err = bb_mdns_cache_build_key("rig.", "abc123", out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("rig.abc123", out);
}

void test_bb_mdns_cache_build_key_null_out_rejected(void)
{
    bb_err_t err = bb_mdns_cache_build_key(NULL, "abc123", NULL, 16);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_cache_build_key_zero_out_size_rejected(void)
{
    char out[BB_MDNS_CACHE_KEY_MAX];
    bb_err_t err = bb_mdns_cache_build_key(NULL, "abc123", out, 0);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_cache_build_key_null_instance_name_rejected(void)
{
    char out[BB_MDNS_CACHE_KEY_MAX];
    bb_err_t err = bb_mdns_cache_build_key(NULL, NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_cache_build_key_empty_instance_name_rejected(void)
{
    char out[BB_MDNS_CACHE_KEY_MAX];
    bb_err_t err = bb_mdns_cache_build_key(NULL, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_cache_build_key_truncation_is_safe(void)
{
    char out[8];
    bb_err_t err = bb_mdns_cache_build_key(NULL, "TaipanMiner-abc123", out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
    // Truncated but always NUL-terminated within out_size.
    TEST_ASSERT_EQUAL_INT('\0', out[sizeof(out) - 1]);
    TEST_ASSERT_TRUE(strlen(out) < sizeof(out));
}

// ---------------------------------------------------------------------------
// bb_mdns_cache_result_valid
// ---------------------------------------------------------------------------

void test_bb_mdns_cache_result_valid_happy_path(void)
{
    TEST_ASSERT_TRUE(bb_mdns_cache_result_valid("TaipanMiner-abc123", "192.168.1.42"));
}

void test_bb_mdns_cache_result_valid_null_instance_name(void)
{
    TEST_ASSERT_FALSE(bb_mdns_cache_result_valid(NULL, "192.168.1.42"));
}

void test_bb_mdns_cache_result_valid_empty_instance_name(void)
{
    TEST_ASSERT_FALSE(bb_mdns_cache_result_valid("", "192.168.1.42"));
}

void test_bb_mdns_cache_result_valid_null_ip4(void)
{
    TEST_ASSERT_FALSE(bb_mdns_cache_result_valid("TaipanMiner-abc123", NULL));
}

void test_bb_mdns_cache_result_valid_empty_ip4(void)
{
    TEST_ASSERT_FALSE(bb_mdns_cache_result_valid("TaipanMiner-abc123", ""));
}

void test_bb_mdns_cache_result_valid_malformed_ip4(void)
{
    TEST_ASSERT_FALSE(bb_mdns_cache_result_valid("TaipanMiner-abc123", "not-an-ip"));
}

void test_bb_mdns_cache_result_valid_ip4_with_letter(void)
{
    // 'a' (> '9', not < '0') exercises the "greater than '9'" branch.
    TEST_ASSERT_FALSE(bb_mdns_cache_result_valid("TaipanMiner-abc123", "192.168.1.a"));
}

void test_bb_mdns_cache_result_valid_ip4_with_low_char(void)
{
    // Space (0x20, < '0', not '.') exercises the "less than '0'" branch.
    TEST_ASSERT_FALSE(bb_mdns_cache_result_valid("TaipanMiner-abc123", "192 168.1.1"));
}
