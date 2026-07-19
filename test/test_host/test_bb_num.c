#include "unity.h"
#include "bb_num.h"

#include <stdint.h>

// bb_clampi — clamp x into [lo, hi].

void test_bb_clampi_below_range_returns_lo(void)
{
    TEST_ASSERT_EQUAL_INT32(0, bb_clampi(-5, 0, 10));
}

void test_bb_clampi_above_range_returns_hi(void)
{
    TEST_ASSERT_EQUAL_INT32(10, bb_clampi(15, 0, 10));
}

void test_bb_clampi_in_range_returns_x(void)
{
    TEST_ASSERT_EQUAL_INT32(5, bb_clampi(5, 0, 10));
}

void test_bb_clampi_equal_to_lo_returns_lo(void)
{
    TEST_ASSERT_EQUAL_INT32(0, bb_clampi(0, 0, 10));
}

void test_bb_clampi_equal_to_hi_returns_hi(void)
{
    TEST_ASSERT_EQUAL_INT32(10, bb_clampi(10, 0, 10));
}

// bb_clampf — clamp x into [lo, hi].

void test_bb_clampf_below_range_returns_lo(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, bb_clampf(-5.0f, 0.0f, 10.0f));
}

void test_bb_clampf_above_range_returns_hi(void)
{
    TEST_ASSERT_EQUAL_FLOAT(10.0f, bb_clampf(15.0f, 0.0f, 10.0f));
}

void test_bb_clampf_in_range_returns_x(void)
{
    TEST_ASSERT_EQUAL_FLOAT(5.5f, bb_clampf(5.5f, 0.0f, 10.0f));
}

void test_bb_clampf_equal_to_lo_returns_lo(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, bb_clampf(0.0f, 0.0f, 10.0f));
}

void test_bb_clampf_equal_to_hi_returns_hi(void)
{
    TEST_ASSERT_EQUAL_FLOAT(10.0f, bb_clampf(10.0f, 0.0f, 10.0f));
}

// bb_num_u64_to_dec -- portable u64 -> decimal, no snprintf/locale/`ll`-
// format dependency.

void test_bb_num_u64_to_dec_zero(void)
{
    char buf[8];
    size_t n = bb_num_u64_to_dec(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

void test_bb_num_u64_to_dec_single_digit(void)
{
    char buf[8];
    size_t n = bb_num_u64_to_dec(buf, sizeof(buf), 7);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_STRING("7", buf);
}

void test_bb_num_u64_to_dec_max(void)
{
    char buf[21];
    size_t n = bb_num_u64_to_dec(buf, sizeof(buf), UINT64_MAX);
    TEST_ASSERT_EQUAL_UINT(20, n);
    TEST_ASSERT_EQUAL_STRING("18446744073709551615", buf);
}

void test_bb_num_u64_to_dec_zero_cap_is_noop(void)
{
    char buf[8] = { 'X' };
    size_t n = bb_num_u64_to_dec(buf, 0, 42);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL('X', buf[0]);  // untouched
}

void test_bb_num_u64_to_dec_cap_one_writes_only_nul(void)
{
    char buf[8] = { 'X' };
    size_t n = bb_num_u64_to_dec(buf, 1, 42);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL('\0', buf[0]);
}

void test_bb_num_u64_to_dec_truncates_and_nul_terminates(void)
{
    char buf[3];  // room for 2 digits + NUL
    size_t n = bb_num_u64_to_dec(buf, sizeof(buf), 123456789ULL);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_STRING("12", buf);
}

// bb_num_i64_to_dec -- signed counterpart; INT64_MIN handled without
// negation overflow.

void test_bb_num_i64_to_dec_zero(void)
{
    char buf[8];
    size_t n = bb_num_i64_to_dec(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

void test_bb_num_i64_to_dec_positive(void)
{
    char buf[8];
    size_t n = bb_num_i64_to_dec(buf, sizeof(buf), 42);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_STRING("42", buf);
}

void test_bb_num_i64_to_dec_negative(void)
{
    char buf[8];
    size_t n = bb_num_i64_to_dec(buf, sizeof(buf), -42);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_STRING("-42", buf);
}

void test_bb_num_i64_to_dec_int64_min(void)
{
    char buf[21];
    size_t n = bb_num_i64_to_dec(buf, sizeof(buf), INT64_MIN);
    TEST_ASSERT_EQUAL_UINT(20, n);
    TEST_ASSERT_EQUAL_STRING("-9223372036854775808", buf);
}

void test_bb_num_i64_to_dec_int64_max(void)
{
    char buf[21];
    size_t n = bb_num_i64_to_dec(buf, sizeof(buf), INT64_MAX);
    TEST_ASSERT_EQUAL_UINT(19, n);
    TEST_ASSERT_EQUAL_STRING("9223372036854775807", buf);
}

void test_bb_num_i64_to_dec_zero_cap_is_noop(void)
{
    char buf[8] = { 'X' };
    size_t n = bb_num_i64_to_dec(buf, 0, -5);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL('X', buf[0]);  // untouched
}

void test_bb_num_i64_to_dec_negative_cap_one_writes_only_nul(void)
{
    char buf[8] = { 'X' };
    size_t n = bb_num_i64_to_dec(buf, 1, -5);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL('\0', buf[0]);
}

void test_bb_num_i64_to_dec_negative_truncates_to_sign_only(void)
{
    char buf[2];  // room for '-' + NUL only
    size_t n = bb_num_i64_to_dec(buf, sizeof(buf), -42);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_STRING("-", buf);
}

// bb_num_bswap32 — reverse byte order of a 32-bit word.

void test_bb_num_bswap32_known_vector(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x04030201u, bb_num_bswap32(0x01020304u));
}

void test_bb_num_bswap32_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, bb_num_bswap32(0u));
}

void test_bb_num_bswap32_all_ones(void)
{
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, bb_num_bswap32(0xFFFFFFFFu));
}

// bb_num_bswap32_words — per-4-byte-group in-place reversal, NOT a
// whole-buffer reversal. Vector: a real 32-byte stratum prevhash (8 groups
// of 4 bytes each), verifying only intra-group byte order flips.

void test_bb_num_bswap32_words_stratum_prevhash_sample(void)
{
    uint8_t buf[32] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13,
        0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b,
        0x1c, 0x1d, 0x1e, 0x1f,
    };
    const uint8_t expected[32] = {
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x06, 0x05, 0x04,
        0x0b, 0x0a, 0x09, 0x08,
        0x0f, 0x0e, 0x0d, 0x0c,
        0x13, 0x12, 0x11, 0x10,
        0x17, 0x16, 0x15, 0x14,
        0x1b, 0x1a, 0x19, 0x18,
        0x1f, 0x1e, 0x1d, 0x1c,
    };

    bb_num_bswap32_words(buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(buf));
}

void test_bb_num_bswap32_words_single_group(void)
{
    uint8_t buf[4] = { 0x01, 0x02, 0x03, 0x04 };

    bb_num_bswap32_words(buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT8(0x04, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[3]);
}

void test_bb_num_bswap32_words_zero_len_is_noop(void)
{
    uint8_t buf[4] = { 0x01, 0x02, 0x03, 0x04 };

    bb_num_bswap32_words(buf, 0);

    TEST_ASSERT_EQUAL_UINT8(0x01, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x04, buf[3]);
}

void test_bb_num_bswap32_words_non_multiple_of_4_leaves_trailing_bytes_untouched(void)
{
    // len=6: one full 4-byte group (reversed) + 2 trailing bytes
    // (untouched, per the "len must be a multiple of 4" contract).
    uint8_t buf[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };

    bb_num_bswap32_words(buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT8(0x04, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[3]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x05, buf[4], "trailing partial group must be untouched");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x06, buf[5], "trailing partial group must be untouched");
}
