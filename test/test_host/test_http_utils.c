#include "unity.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "bb_http_server.h"
#include <string.h>

void test_url_decode_basic(void)
{
    char out[256];
    bb_url_decode_field("ssid=TestNetwork&pass=secret", "ssid", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("TestNetwork", out);
}

void test_url_decode_plus_as_space(void)
{
    char out[256];
    bb_url_decode_field("ssid=Test+Network", "ssid", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Test Network", out);
}

void test_url_decode_hex_decode(void)
{
    char out[256];
    bb_url_decode_field("pass=hello%21world", "pass", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("hello!world", out);
}

void test_url_decode_missing_field(void)
{
    char out[256];
    bb_url_decode_field("ssid=TestNetwork", "pass", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_url_decode_truncation(void)
{
    char out[5];
    bb_url_decode_field("ssid=abcdefghij", "ssid", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("abcd", out);
}

void test_url_decode_percent_at_end(void)
{
    char out[256];
    bb_url_decode_field("f=abc%2", "f", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("abc%2", out);
}

void test_bb_url_decode_field_not_first(void)
{
    char out[256];
    bb_url_decode_field("a=1&b=2&c=3", "b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

void test_url_decode_field_not_first(void)
{
    char out[256];
    bb_url_decode_field("a=1&b=2&c=3", "b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

void test_url_decode_empty_value(void)
{
    char out[256];
    bb_url_decode_field("ssid=&pass=secret", "ssid", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_url_decode_field_at_end(void)
{
    char out[256];
    bb_url_decode_field("a=1&b=2", "b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

void test_bb_url_decode_field_at_end(void)
{
    char out[256];
    bb_url_decode_field("a=1&b=2", "b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

// bb_url_parse_bool/bb_url_parse_uint were migrated onto
// bb_scalar_parse_bool/bb_scalar_parse_uint (components/bb_scalar) and
// deleted from this file; their accept/reject coverage lives in
// test/test_host/test_bb_scalar.c.

// ---------------------------------------------------------------------------
// bb_http_send_json_error (bb_http_section PR review, MEDIUM finding) --
// the ONE copy of the set-type+set-status+sendstr idiom that
// bb_http_section_dispatch.c's and bb_diag_section_dispatch.c's own
// respond_error() static helpers used to hand-roll as VERBATIM duplicates
// of each other. Exercised directly here (production and both dispatchers
// share this one implementation).
// ---------------------------------------------------------------------------

void test_bb_http_send_json_error_sets_status_type_and_body(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_err_t rc = bb_http_send_json_error(req, 404, "{\"error\":\"not found\"}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(404, cap.status);
    TEST_ASSERT_EQUAL_STRING("application/json", cap.content_type);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"not found\"}", cap.body);

    bb_http_host_capture_free(&cap);
}
