// Parity smoke test: bb_pub_register_source and bb_pub_register_source_ex
// produce the same publishing behavior when no tags / no subscribe filter.
#include "unity.h"
#include "bb_pub.h"
#include "test_hostname_seed.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Sample helpers
// ---------------------------------------------------------------------------

static bool sample_a(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "a", 42.0);
    return true;
}

// ---------------------------------------------------------------------------
// Capture helpers
// ---------------------------------------------------------------------------

static int s_publish_count = 0;

static bb_err_t count_publish(void *ctx, const char *topic, const char *payload,
                               int len, bool retain)
{
    (void)ctx;
    (void)topic;
    (void)payload;
    (void)len;
    (void)retain;
    s_publish_count++;
    return BB_OK;
}

static void setup_test(void)
{
    // Global setUp already calls bb_pub_test_reset().
    s_publish_count = 0;
    bb_test_seed_hostname("parityhost");
}

// ---------------------------------------------------------------------------
// Parity tests
// ---------------------------------------------------------------------------

void test_bb_pub_parity_register_source_ex_null_tags_same_behavior(void)
{
    setup_test();
    bb_pub_sink_t sink = { .publish = count_publish };
    bb_pub_set_sink(&sink);

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source_ex("a", sample_a, NULL, NULL, 0));
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL(1, s_publish_count);
}

void test_bb_pub_parity_source_and_source_ex_both_publish(void)
{
    setup_test();
    bb_pub_sink_t sink = { .publish = count_publish };
    bb_pub_set_sink(&sink);

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source("a", sample_a, NULL));
    const char *tags[] = { "x" };
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source_ex("b", sample_a, NULL, tags, 1));
    bb_pub_tick_once();
    // Both sources publish: count should be 2 (one per source, one sink).
    TEST_ASSERT_EQUAL(2, s_publish_count);
}

void test_bb_pub_parity_source_info_ex_null_tags_returns_zero(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source("a", sample_a, NULL));
    int ntags = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_source_info_ex(0, NULL, NULL, NULL, NULL, NULL, NULL, &ntags));
    TEST_ASSERT_EQUAL(0, ntags);
}

void test_bb_pub_parity_subscription_predicate_null_ctx_pass_all(void)
{
    // When ctx is NULL, bb_pub_subscription_predicate treats sub as NULL → pass all.
    // (NULL bb_pub_subscription_t pointer → match all)
    TEST_ASSERT_TRUE(bb_pub_subscription_predicate("power", NULL, 0, NULL));
}
