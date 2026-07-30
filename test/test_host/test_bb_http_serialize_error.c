// Host tests for bb_http_serialize_send_error() (B1-1286) -- the shared
// {"error":"<msg>"} response helper factored out of the three per-file
// send_400/send_500/send_501-style helpers it replaces (bb_system_routes.c/
// bb_wifi_http_routes.c/bb_storage_http_routes.c). Driven against the host
// capture harness bb_http_serialize_stream's own tests use.

#include "unity.h"
#include "bb_http_serialize_error.h"
#include "bb_http_host.h"

#include <string.h>

static bb_http_request_t *s_req;
static bb_http_host_capture_t s_cap;

static void cap_begin(void) { bb_http_host_capture_begin(&s_req); }
static void cap_end(void)   { bb_http_host_capture_end(s_req, &s_cap); }
static void cap_free(void)  { bb_http_host_capture_free(&s_cap); }

void test_http_serialize_send_error_happy_path(void)
{
    cap_begin();
    bb_err_t err = bb_http_serialize_send_error(s_req, 400, "invalid JSON");
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(400, s_cap.status);
    TEST_ASSERT_EQUAL_STRING("application/json", s_cap.content_type);
    TEST_ASSERT_NOT_NULL(s_cap.body);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"invalid JSON\"}", s_cap.body);
    cap_free();
}

void test_http_serialize_send_error_null_req_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_serialize_send_error(NULL, 500, "boom"));
}

// bb_http_resp_set_status()'s host stub rejects a status code outside
// [100,599] with BB_ERR_INVALID_ARG (bb_http_status_line()'s own
// range-reject) -- exercises bb_http_serialize_send_error()'s downstream-
// failure propagation without a dedicated force-fail hook.
void test_http_serialize_send_error_invalid_status_propagates(void)
{
    cap_begin();
    bb_err_t err = bb_http_serialize_send_error(s_req, 0, "unreachable status");
    cap_end();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_NULL(s_cap.body);  // nothing written
    cap_free();
}

void test_http_serialize_send_error_null_msg_renders_empty(void)
{
    cap_begin();
    bb_err_t err = bb_http_serialize_send_error(s_req, 500, NULL);
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(500, s_cap.status);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"\"}", s_cap.body);
    cap_free();
}

// A message longer than BB_HTTP_SERIALIZE_ERROR_MSG_MAX is truncated to fit
// (strncpy into the fixed-size snapshot buffer), never overflows it.
void test_http_serialize_send_error_long_msg_truncates(void)
{
    char long_msg[BB_HTTP_SERIALIZE_ERROR_MSG_MAX + 64];
    memset(long_msg, 'x', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    cap_begin();
    bb_err_t err = bb_http_serialize_send_error(s_req, 400, long_msg);
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    // body is `{"error":"` + (MAX-1) x's + `"}`
    size_t expected_x_count = BB_HTTP_SERIALIZE_ERROR_MSG_MAX - 1;
    TEST_ASSERT_EQUAL_size_t(strlen("{\"error\":\"") + expected_x_count + strlen("\"}"), s_cap.body_len);
    cap_free();
}
