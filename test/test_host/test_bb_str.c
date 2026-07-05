#include "unity.h"
#include "bb_str.h"

#include <string.h>

// Table-driven cases exercising bb_strlcpy's normal (non-zero-dstsize) path:
// exact-fit, truncation, empty src, and a normal shorter copy.
typedef struct {
    const char *name;
    const char *src;
    size_t dstsize;
    const char *expected_dst;
    size_t expected_ret;
} bb_str_case_t;

static const bb_str_case_t s_cases[] = {
    // Exact fit: src fits with exactly one byte to spare for the NUL.
    { "exact_fit",       "abc",  4, "abc",  3 },
    // Truncation: dstsize too small — ret >= dstsize signals truncation,
    // and dst is NUL-terminated at dstsize-1.
    { "truncation",      "abcdef", 4, "abc", 6 },
    // Empty src: dst becomes "".
    { "empty_src",       "",     8, "",     0 },
    // Normal copy shorter than dstsize.
    { "shorter_than_cap","hi",  16, "hi",   2 },
};

void test_bb_strlcpy_table_driven(void)
{
    char dst[32];

    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        const bb_str_case_t *tc = &s_cases[i];

        memset(dst, 'X', sizeof(dst));
        size_t ret = bb_strlcpy(dst, tc->src, tc->dstsize);

        TEST_ASSERT_EQUAL_STRING_MESSAGE(tc->expected_dst, dst, tc->name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(tc->expected_ret, ret, tc->name);
    }
}

void test_bb_strlcpy_truncation_is_detected_via_return_value(void)
{
    char dst[4];
    size_t ret = bb_strlcpy(dst, "abcdef", sizeof(dst));

    TEST_ASSERT_TRUE(ret >= sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("abc", dst);
    TEST_ASSERT_EQUAL_UINT(6, ret);
}

void test_bb_strlcpy_exact_fit_is_not_flagged_as_truncated(void)
{
    char dst[4];
    size_t ret = bb_strlcpy(dst, "abc", sizeof(dst));

    TEST_ASSERT_FALSE(ret >= sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("abc", dst);
    TEST_ASSERT_EQUAL_UINT(3, ret);
}

void test_bb_strlcpy_dstsize_zero_writes_nothing_and_returns_strlen(void)
{
    // dst == NULL is only valid when dstsize == 0.
    size_t ret = bb_strlcpy(NULL, "hello", 0);

    TEST_ASSERT_EQUAL_UINT(5, ret);
}

void test_bb_strlcpy_dstsize_zero_with_empty_src_returns_zero(void)
{
    size_t ret = bb_strlcpy(NULL, "", 0);

    TEST_ASSERT_EQUAL_UINT(0, ret);
}

void test_bb_strlcpy_empty_src_terminates_dst_as_empty_string(void)
{
    char dst[8];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_strlcpy(dst, "", sizeof(dst));

    TEST_ASSERT_EQUAL_STRING("", dst);
    TEST_ASSERT_EQUAL_UINT(0, ret);
}

void test_bb_strlcpy_dstsize_one_writes_only_nul_terminator(void)
{
    char dst[4];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_strlcpy(dst, "abc", 1);

    TEST_ASSERT_EQUAL_STRING("", dst);
    TEST_ASSERT_EQUAL_UINT(3, ret);
}

// bb_str_field — fixed-width, length-delimited field fill. strncpy(dst,
// src, dstsize) semantics: copy + zero-pad tail, no forced NUL.

void test_bb_str_field_shorter_src_copies_and_zero_pads_tail(void)
{
    char dst[8];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_str_field(dst, "hi", sizeof(dst));

    TEST_ASSERT_EQUAL_UINT(2, ret);
    TEST_ASSERT_EQUAL_UINT8('h', dst[0]);
    TEST_ASSERT_EQUAL_UINT8('i', dst[1]);
    for (size_t i = 2; i < sizeof(dst); i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dst[i], "tail must be zero-padded");
    }
}

void test_bb_str_field_src_exactly_fills_dstsize_no_nul_no_pad(void)
{
    char dst[4];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_str_field(dst, "abcd", sizeof(dst));

    TEST_ASSERT_EQUAL_UINT(4, ret);
    TEST_ASSERT_EQUAL_UINT8('a', dst[0]);
    TEST_ASSERT_EQUAL_UINT8('b', dst[1]);
    TEST_ASSERT_EQUAL_UINT8('c', dst[2]);
    TEST_ASSERT_EQUAL_UINT8('d', dst[3]);
}

void test_bb_str_field_longer_src_truncates_no_nul(void)
{
    char dst[4];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_str_field(dst, "abcdef", sizeof(dst));

    TEST_ASSERT_EQUAL_UINT(6, ret);
    TEST_ASSERT_EQUAL_UINT8('a', dst[0]);
    TEST_ASSERT_EQUAL_UINT8('b', dst[1]);
    TEST_ASSERT_EQUAL_UINT8('c', dst[2]);
    TEST_ASSERT_EQUAL_UINT8('d', dst[3]);
}

void test_bb_str_field_dstsize_zero_writes_nothing_and_returns_strlen(void)
{
    size_t ret = bb_str_field(NULL, "hello", 0);

    TEST_ASSERT_EQUAL_UINT(5, ret);
}

void test_bb_str_field_empty_src_zero_pads_entire_dst(void)
{
    char dst[8];
    memset(dst, 'X', sizeof(dst));

    size_t ret = bb_str_field(dst, "", sizeof(dst));

    TEST_ASSERT_EQUAL_UINT(0, ret);
    for (size_t i = 0; i < sizeof(dst); i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dst[i], "dst must be all-zero");
    }
}

void test_bb_str_field_return_value_equals_strlen_src_in_each_case(void)
{
    char dst[8];

    TEST_ASSERT_EQUAL_UINT(strlen("hi"), bb_str_field(dst, "hi", sizeof(dst)));
    TEST_ASSERT_EQUAL_UINT(strlen("abcd"), bb_str_field(dst, "abcd", 4));
    TEST_ASSERT_EQUAL_UINT(strlen("abcdef"), bb_str_field(dst, "abcdef", 4));
    TEST_ASSERT_EQUAL_UINT(strlen("hello"), bb_str_field(NULL, "hello", 0));
    TEST_ASSERT_EQUAL_UINT(strlen(""), bb_str_field(dst, "", sizeof(dst)));
}
