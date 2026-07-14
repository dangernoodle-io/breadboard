#include "unity.h"
#include "bb_http_query.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
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

// ===========================================================================
// bb_http_req_query_key_value / bb_http_req_query_has_key (host backend,
// multi-param query_string branch of platform/host/bb_http_server/bb_http_host.c)
//
// B1-...: lost their only (indirect) coverage when the bb_pub/bb_sink_*
// cluster was deleted — nothing on host called bb_http_host_capture_set_query_string
// before that. Still has live ESP-IDF callers (bb_cache_routes, bb_event_routes,
// bb_diag_routes all parse "&"-separated query strings) — direct tests here
// restore real coverage of the parser itself.
// ===========================================================================

void test_bb_http_req_query_key_value_no_active_capture_returns_invalid_arg(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_t cap = {0};
    bb_http_host_capture_end(req, &cap);   // disarms the slot
    bb_http_host_capture_free(&cap);

    // req is now a stale pointer — no active capture matches it.
    char out[8];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_req_query_key_value(req, "a", out, sizeof(out)));
}

void test_bb_http_req_query_key_value_null_key_returns_invalid_arg(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    char out[8];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_req_query_key_value(req, NULL, out, sizeof(out)));
}

void test_bb_http_req_query_key_value_single_param(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("format=json");

    char out[16] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_req_query_key_value(req, "format", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("json", out);
}

void test_bb_http_req_query_key_value_key_at_start(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("a=1&b=2&c=3");

    char out[8] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_req_query_key_value(req, "a", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1", out);
}

void test_bb_http_req_query_key_value_key_at_middle(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("a=1&b=2&c=3");

    char out[8] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_req_query_key_value(req, "b", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

void test_bb_http_req_query_key_value_key_at_end(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("a=1&b=2&c=3");

    char out[8] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_req_query_key_value(req, "c", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("3", out);
}

void test_bb_http_req_query_key_value_missing_key_returns_invalid_arg(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("a=1&b=2");

    char out[8] = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_req_query_key_value(req, "z", out, sizeof(out)));
}

void test_bb_http_req_query_key_value_missing_key_trailing_ampersand(void)
{
    // Malformed/trailing "&" — the tail after the last '&' is an empty
    // segment; the parser must terminate the loop cleanly rather than
    // matching or looping forever.
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("a=1&");

    char out[8] = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_req_query_key_value(req, "z", out, sizeof(out)));
}

void test_bb_http_req_query_key_value_empty_value(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("a=&b=2");

    char out[8] = {0xff, 0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_req_query_key_value(req, "a", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_bb_http_req_query_key_value_bare_key_no_equals(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("schema&format=json");

    char out[8] = {0xff, 0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_req_query_key_value(req, "schema", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_bb_http_req_query_key_value_key_prefix_of_another_key_not_falsely_matched(void)
{
    // "flagged" is checked first; the parser must NOT treat it as a match for
    // "flag" (no trailing '=' right after the prefix) and must keep scanning
    // to find the real "flag=2" segment.
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("flagged=1&flag=2");

    char out[8] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_req_query_key_value(req, "flag", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

void test_bb_http_req_query_key_value_no_query_injected_returns_invalid_arg(void)
{
    // Neither the multi-param query_string nor the legacy single key/value
    // was injected — must fall through to the final BB_ERR_INVALID_ARG
    // (distinct from the "found but too small" / "not found in query_string"
    // failure paths above; this exercises the legacy-fallback guard itself).
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    char out[8] = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_req_query_key_value(req, "anything", out, sizeof(out)));
}

void test_bb_http_req_query_key_value_buffer_too_small_returns_invalid_arg(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("a=hello");

    char out[3];  // too small for "hello" + NUL (needs 6)
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_req_query_key_value(req, "a", out, sizeof(out)));
}

void test_bb_http_req_query_has_key_no_active_capture_returns_false(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_t cap = {0};
    bb_http_host_capture_end(req, &cap);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_FALSE(bb_http_req_query_has_key(req, "a"));
}

void test_bb_http_req_query_has_key_null_key_returns_false(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    TEST_ASSERT_FALSE(bb_http_req_query_has_key(req, NULL));
}

void test_bb_http_req_query_has_key_multi_param_present(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("schema&format=json");
    TEST_ASSERT_TRUE(bb_http_req_query_has_key(req, "format"));
    TEST_ASSERT_TRUE(bb_http_req_query_has_key(req, "schema"));
}

void test_bb_http_req_query_has_key_multi_param_absent(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("schema&format=json");
    TEST_ASSERT_FALSE(bb_http_req_query_has_key(req, "missing"));
}

void test_bb_http_req_query_has_key_legacy_single_param_present(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_param("format", "json");
    TEST_ASSERT_TRUE(bb_http_req_query_has_key(req, "format"));
}

void test_bb_http_req_query_has_key_legacy_single_param_absent(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_param("format", "json");
    TEST_ASSERT_FALSE(bb_http_req_query_has_key(req, "other"));
}
