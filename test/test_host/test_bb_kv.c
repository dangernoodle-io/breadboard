#include "unity.h"
#include "bb_kv.h"

#include <string.h>

#define BB_KV_TEST_MAX_PAIRS 8

typedef struct {
    int count;
    char keys[BB_KV_TEST_MAX_PAIRS][32];
    char vals[BB_KV_TEST_MAX_PAIRS][32];
} bb_kv_test_ctx_t;

static void bb_kv_test_reset(bb_kv_test_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static void bb_kv_test_collect(const char *key, size_t key_len,
                                const char *val, size_t val_len, void *ctx_)
{
    bb_kv_test_ctx_t *ctx = (bb_kv_test_ctx_t *)ctx_;
    TEST_ASSERT_LESS_THAN(BB_KV_TEST_MAX_PAIRS, ctx->count);

    TEST_ASSERT_LESS_THAN(sizeof(ctx->keys[0]), key_len + 1);
    memcpy(ctx->keys[ctx->count], key, key_len);
    ctx->keys[ctx->count][key_len] = '\0';

    TEST_ASSERT_LESS_THAN(sizeof(ctx->vals[0]), val_len + 1);
    memcpy(ctx->vals[ctx->count], val, val_len);
    ctx->vals[ctx->count][val_len] = '\0';

    ctx->count++;
}

void test_bb_kv_parse_null_string_invokes_no_callbacks(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse(NULL, bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_kv_parse_empty_string_invokes_no_callbacks(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_kv_parse_null_callback_is_safe_noop(void)
{
    // Must not crash / OOB-read even though the string has valid entries.
    bb_kv_parse("a=1,b=2", NULL, NULL);
}

void test_bb_kv_parse_single_pair(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("a=1", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
}

void test_bb_kv_parse_multi_pair(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("a=1,b=2,c=3", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.keys[1]);
    TEST_ASSERT_EQUAL_STRING("2", ctx.vals[1]);
    TEST_ASSERT_EQUAL_STRING("c", ctx.keys[2]);
    TEST_ASSERT_EQUAL_STRING("3", ctx.vals[2]);
}

void test_bb_kv_parse_trims_surrounding_whitespace(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("  a = 1 ,  b=2 ", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.keys[1]);
    TEST_ASSERT_EQUAL_STRING("2", ctx.vals[1]);
}

void test_bb_kv_parse_trims_tab_whitespace(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("\ta\t=\t1\t,b=2", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.keys[1]);
    TEST_ASSERT_EQUAL_STRING("2", ctx.vals[1]);
}

void test_bb_kv_parse_empty_value_is_valid(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("a=", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("", ctx.vals[0]);
}

void test_bb_kv_parse_empty_key_is_skipped(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("=1", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_kv_parse_whitespace_only_key_is_skipped(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("  =1", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_kv_parse_entry_with_no_equals_is_skipped(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("abc", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

void test_bb_kv_parse_trailing_comma_is_skipped(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("a=1,", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
}

void test_bb_kv_parse_leading_comma_is_skipped(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse(",a=1", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
}

void test_bb_kv_parse_double_comma_is_skipped(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("a=1,,b=2", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("1", ctx.vals[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.keys[1]);
    TEST_ASSERT_EQUAL_STRING("2", ctx.vals[1]);
}

void test_bb_kv_parse_key_and_value_with_internal_spaces(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("my key=my val", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("my key", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("my val", ctx.vals[0]);
}

void test_bb_kv_parse_last_pair_with_no_trailing_comma(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("a=1,b=2,c=3", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("c", ctx.keys[2]);
    TEST_ASSERT_EQUAL_STRING("3", ctx.vals[2]);
}

void test_bb_kv_parse_value_at_string_end_no_delimiters(void)
{
    bb_kv_test_ctx_t ctx;
    bb_kv_test_reset(&ctx);

    bb_kv_parse("tag=verbose", bb_kv_test_collect, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("tag", ctx.keys[0]);
    TEST_ASSERT_EQUAL_STRING("verbose", ctx.vals[0]);
}
