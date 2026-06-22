#include "unity.h"
#include "bb_http_query.h"
#include <stddef.h>

void test_bb_http_query_token_present_bare(void)
{
    TEST_ASSERT_TRUE(bb_http_query_token_present("a=1&flag&b=2", "flag"));
    TEST_ASSERT_TRUE(bb_http_query_token_present("schema", "schema"));
}

void test_bb_http_query_token_present_with_value(void)
{
    TEST_ASSERT_TRUE(bb_http_query_token_present("schema=1&format=json", "format"));
    TEST_ASSERT_TRUE(bb_http_query_token_present("format=prom", "format"));
}

void test_bb_http_query_token_present_absent(void)
{
    TEST_ASSERT_FALSE(bb_http_query_token_present("a=1&b=2", "c"));
    TEST_ASSERT_FALSE(bb_http_query_token_present("", "x"));
    // segment shorter than key (seg < klen branch)
    TEST_ASSERT_FALSE(bb_http_query_token_present("a", "flag"));
}

void test_bb_http_query_token_present_prefix_not_whole_token(void)
{
    TEST_ASSERT_FALSE(bb_http_query_token_present("flagged=1", "flag"));
    TEST_ASSERT_FALSE(bb_http_query_token_present("flag=1", "fl"));
}

void test_bb_http_query_token_present_null_safe(void)
{
    TEST_ASSERT_FALSE(bb_http_query_token_present(NULL, "x"));
    TEST_ASSERT_FALSE(bb_http_query_token_present("x", NULL));
}
