#include "unity.h"
#include "bb_fmt.h"

#include <string.h>

// bb_fmt_hex — hex-encode bytes with optional separator, bb_strlcpy-style
// truncation semantics (return value >= dstsize signals truncation).

void test_bb_fmt_hex_zero_nbytes_returns_zero_and_terminates(void)
{
    char dst[8];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_fmt_hex(NULL, 0, ':', dst, sizeof(dst));

    TEST_ASSERT_EQUAL_UINT(0, ret);
    TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_bb_fmt_hex_with_separator(void)
{
    static const uint8_t bytes[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    char dst[16];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_fmt_hex(bytes, sizeof(bytes), ':', dst, sizeof(dst));

    TEST_ASSERT_EQUAL_STRING("de:ad:be:ef", dst);
    TEST_ASSERT_EQUAL_UINT(strlen("de:ad:be:ef"), ret);
}

void test_bb_fmt_hex_without_separator(void)
{
    static const uint8_t bytes[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    char dst[16];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_fmt_hex(bytes, sizeof(bytes), 0, dst, sizeof(dst));

    TEST_ASSERT_EQUAL_STRING("deadbeef", dst);
    TEST_ASSERT_EQUAL_UINT(strlen("deadbeef"), ret);
}

void test_bb_fmt_hex_exact_fit_dstsize(void)
{
    static const uint8_t bytes[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    char dst[sizeof("deadbeef")];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_fmt_hex(bytes, sizeof(bytes), 0, dst, sizeof(dst));

    TEST_ASSERT_EQUAL_STRING("deadbeef", dst);
    TEST_ASSERT_EQUAL_UINT(strlen(dst), ret);
    TEST_ASSERT_FALSE(ret >= sizeof(dst));
}

void test_bb_fmt_hex_truncation_is_detected_via_return_value(void)
{
    static const uint8_t bytes[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    char dst[5];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_fmt_hex(bytes, sizeof(bytes), ':', dst, sizeof(dst));

    TEST_ASSERT_TRUE(ret >= sizeof(dst));
    TEST_ASSERT_EQUAL_UINT(strlen("de:ad:be:ef"), ret);
    TEST_ASSERT_EQUAL_STRING("de:a", dst);
}

void test_bb_fmt_hex_dstsize_zero_writes_nothing_and_returns_full_length(void)
{
    static const uint8_t bytes[] = { 0xDE, 0xAD, 0xBE, 0xEF };

    // dst == NULL is only valid when dstsize == 0.
    size_t ret = bb_fmt_hex(bytes, sizeof(bytes), ':', NULL, 0);

    TEST_ASSERT_EQUAL_UINT(strlen("de:ad:be:ef"), ret);
}

void test_bb_fmt_hex_dstsize_one_writes_only_nul_terminator(void)
{
    static const uint8_t bytes[] = { 0xDE, 0xAD };
    char dst[4];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_fmt_hex(bytes, sizeof(bytes), ':', dst, 1);

    TEST_ASSERT_EQUAL_STRING("", dst);
    TEST_ASSERT_EQUAL_UINT(strlen("de:ad"), ret);
}

// bb_fmt_mac6 — convenience wrapper over bb_fmt_hex for 6-byte MACs.

void test_bb_fmt_mac6_known_mac_formats_exactly(void)
{
    static const uint8_t mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    char dst[18];
    memset(dst, 'X', sizeof(dst));

    bool ok = bb_fmt_mac6(mac, dst, sizeof(dst));

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("00:11:22:33:44:55", dst);
}

void test_bb_fmt_mac6_dstsize_too_small_returns_false(void)
{
    static const uint8_t mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    char dst[17];

    bool ok = bb_fmt_mac6(mac, dst, sizeof(dst));

    TEST_ASSERT_FALSE(ok);
}

void test_bb_fmt_mac6_dstsize_larger_than_needed_succeeds(void)
{
    static const uint8_t mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    char dst[19];
    memset(dst, 'X', sizeof(dst));

    bool ok = bb_fmt_mac6(mac, dst, sizeof(dst));

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("00:11:22:33:44:55", dst);
}

// bb_fmt_bool — pure inverse of the codebase's canonical "true"/"false"
// boolean string spellings.

void test_bb_fmt_bool_true(void)
{
    TEST_ASSERT_EQUAL_STRING("true", bb_fmt_bool(true));
}

void test_bb_fmt_bool_false(void)
{
    TEST_ASSERT_EQUAL_STRING("false", bb_fmt_bool(false));
}
