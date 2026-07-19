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

// bb_str_kv_parse — "key=value,key=value" string splitter.

#define BB_STR_KV_TEST_MAX_PAIRS 8

typedef struct {
    int count;
    char keys[BB_STR_KV_TEST_MAX_PAIRS][32];
    char vals[BB_STR_KV_TEST_MAX_PAIRS][32];
} bb_str_kv_test_ctx_t;

static void bb_str_kv_test_reset(bb_str_kv_test_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static void bb_str_kv_test_collect(const char *key, size_t key_len,
                                    const char *val, size_t val_len, void *ctx_)
{
    bb_str_kv_test_ctx_t *ctx = (bb_str_kv_test_ctx_t *)ctx_;
    TEST_ASSERT_LESS_THAN(BB_STR_KV_TEST_MAX_PAIRS, ctx->count);

    TEST_ASSERT_LESS_THAN(sizeof(ctx->keys[0]), key_len + 1);
    memcpy(ctx->keys[ctx->count], key, key_len);
    ctx->keys[ctx->count][key_len] = '\0';

    TEST_ASSERT_LESS_THAN(sizeof(ctx->vals[0]), val_len + 1);
    memcpy(ctx->vals[ctx->count], val, val_len);
    ctx->vals[ctx->count][val_len] = '\0';

    ctx->count++;
}

void test_bb_str_kv_parse_null_string_invokes_no_callbacks(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse(NULL, bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_str_kv_parse_empty_string_invokes_no_callbacks(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_str_kv_parse_null_callback_is_safe_noop(void)
{
    // Must not crash / OOB-read even though the string has valid entries.
    bb_str_kv_parse("a=1,b=2", NULL, NULL);
}

void test_bb_str_kv_parse_single_pair(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("a=1", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
}

void test_bb_str_kv_parse_multi_pair(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("a=1,b=2,c=3", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.keys[1]);
    TEST_ASSERT_EQUAL_STRING("2", ctx.vals[1]);
    TEST_ASSERT_EQUAL_STRING("c", ctx.keys[2]);
    TEST_ASSERT_EQUAL_STRING("3", ctx.vals[2]);
}

void test_bb_str_kv_parse_trims_surrounding_whitespace(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("  a = 1 ,  b=2 ", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.keys[1]);
    TEST_ASSERT_EQUAL_STRING("2", ctx.vals[1]);
}

void test_bb_str_kv_parse_trims_tab_whitespace(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("\ta\t=\t1\t,b=2", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.keys[1]);
    TEST_ASSERT_EQUAL_STRING("2", ctx.vals[1]);
}

void test_bb_str_kv_parse_empty_value_is_valid(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("a=", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("", ctx.vals[0]);
}

void test_bb_str_kv_parse_empty_key_is_skipped(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("=1", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_str_kv_parse_whitespace_only_key_is_skipped(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("  =1", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_str_kv_parse_entry_with_no_equals_is_skipped(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("abc", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_str_kv_parse_trailing_comma_is_skipped(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("a=1,", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
}

void test_bb_str_kv_parse_leading_comma_is_skipped(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse(",a=1", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
}

void test_bb_str_kv_parse_double_comma_is_skipped(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("a=1,,b=2", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.keys[1]);
    TEST_ASSERT_EQUAL_STRING("2", ctx.vals[1]);
}

void test_bb_str_kv_parse_key_and_value_with_internal_spaces(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("my key=my val", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("my key", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("my val", ctx.vals[0]);
}

void test_bb_str_kv_parse_last_pair_with_no_trailing_comma(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("a=1,b=2,c=3", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("c", ctx.keys[2]);
    TEST_ASSERT_EQUAL_STRING("3", ctx.vals[2]);
}

void test_bb_str_kv_parse_value_at_string_end_no_delimiters(void)
{
    bb_str_kv_test_ctx_t ctx;
    bb_str_kv_test_reset(&ctx);

    bb_str_kv_parse("tag=verbose", bb_str_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("tag", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("verbose", ctx.vals[0]);
}

// bb_str_hex_to_bytes / bb_str_bytes_to_hex — hex<->bytes codec.

void test_bb_str_hex_to_bytes_null_returns_zero(void)
{
    uint8_t out[8];
    size_t n = bb_str_hex_to_bytes(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_str_hex_to_bytes_empty_string_returns_zero(void)
{
    uint8_t out[8];
    size_t n = bb_str_hex_to_bytes("", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_str_hex_to_bytes_basic(void)
{
    uint8_t out[4] = { 0 };
    size_t n = bb_str_hex_to_bytes("deadbeef", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_UINT8(0xde, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xad, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0xbe, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0xef, out[3]);
}

void test_bb_str_hex_to_bytes_uppercase(void)
{
    uint8_t out[2] = { 0 };
    size_t n = bb_str_hex_to_bytes("AB12", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT8(0xab, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x12, out[1]);
}

void test_bb_str_hex_to_bytes_odd_length_drops_dangling_nibble(void)
{
    uint8_t out[4] = { 0 };
    size_t n = bb_str_hex_to_bytes("abc", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT8(0xab, out[0]);
}

void test_bb_str_hex_to_bytes_invalid_char_stops_at_prefix(void)
{
    uint8_t out[4] = { 0 };
    size_t n = bb_str_hex_to_bytes("ab zz", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT8(0xab, out[0]);
}

void test_bb_str_hex_to_bytes_invalid_char_in_second_nibble_stops(void)
{
    uint8_t out[4] = { 0 };
    size_t n = bb_str_hex_to_bytes("a!cd", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_str_hex_to_bytes_max_out_truncates(void)
{
    uint8_t out[2] = { 0 };
    size_t n = bb_str_hex_to_bytes("deadbeef", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT8(0xde, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xad, out[1]);
}

void test_bb_str_hex_to_bytes_max_out_zero_writes_nothing(void)
{
    uint8_t out[4] = { 0xff, 0xff, 0xff, 0xff };
    size_t n = bb_str_hex_to_bytes("dead", out, 0);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT8(0xff, out[0]);
}

void test_bb_str_bytes_to_hex_basic(void)
{
    const uint8_t data[] = { 0xde, 0xad, 0xbe, 0xef };
    char hex[16];
    memset(hex, 'X', sizeof(hex));

    size_t n = bb_str_bytes_to_hex(data, sizeof(data), hex, sizeof(hex));

    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_STRING("deadbeef", hex);
}

void test_bb_str_bytes_to_hex_is_lowercase(void)
{
    const uint8_t data[] = { 0xAB, 0xCD, 0xEF };
    char hex[8];

    bb_str_bytes_to_hex(data, sizeof(data), hex, sizeof(hex));

    TEST_ASSERT_EQUAL_STRING("abcdef", hex);
}

void test_bb_str_bytes_to_hex_zero_len(void)
{
    char hex[4];
    memset(hex, 'X', sizeof(hex));

    size_t n = bb_str_bytes_to_hex(NULL, 0, hex, sizeof(hex));

    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_STRING("", hex);
}

void test_bb_str_bytes_to_hex_cap_zero_writes_nothing(void)
{
    const uint8_t data[] = { 0xde, 0xad };
    char hex[4] = { 'X', 'X', 'X', 'X' };

    size_t n = bb_str_bytes_to_hex(data, sizeof(data), hex, 0);

    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL('X', hex[0]);
}

void test_bb_str_bytes_to_hex_cap_one_writes_only_nul(void)
{
    const uint8_t data[] = { 0xde, 0xad };
    char hex[4] = { 'X', 'X', 'X', 'X' };

    size_t n = bb_str_bytes_to_hex(data, sizeof(data), hex, 1);

    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_STRING("", hex);
}

void test_bb_str_bytes_to_hex_cap_truncates_to_whole_pairs(void)
{
    const uint8_t data[] = { 0xde, 0xad, 0xbe, 0xef };
    char hex[4];  // room for 1 pair + NUL, plus one spare byte discarded

    size_t n = bb_str_bytes_to_hex(data, sizeof(data), hex, sizeof(hex));

    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_STRING("de", hex);
}

void test_bb_str_hex_bytes_roundtrip_varied_lengths(void)
{
    const uint8_t vectors[][8] = {
        { 0x00 },
        { 0x01, 0x02, 0x03, 0x04 },
        { 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00 },
    };
    const size_t lens[] = { 1, 4, 8 };

    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        char hex[32];
        uint8_t decoded[8] = { 0 };

        size_t hex_n = bb_str_bytes_to_hex(vectors[i], lens[i], hex, sizeof(hex));
        TEST_ASSERT_EQUAL_UINT(lens[i], hex_n);

        size_t bytes_n = bb_str_hex_to_bytes(hex, decoded, sizeof(decoded));
        TEST_ASSERT_EQUAL_UINT(lens[i], bytes_n);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(vectors[i], decoded, lens[i]);
    }
}

void test_bb_str_hex_bytes_roundtrip_zero_length(void)
{
    char hex[4];
    memset(hex, 'X', sizeof(hex));

    size_t hex_n = bb_str_bytes_to_hex(NULL, 0, hex, sizeof(hex));
    TEST_ASSERT_EQUAL_UINT(0, hex_n);
    TEST_ASSERT_EQUAL_STRING("", hex);

    uint8_t decoded[4] = { 0xff, 0xff, 0xff, 0xff };
    size_t bytes_n = bb_str_hex_to_bytes(hex, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL_UINT(0, bytes_n);
}
