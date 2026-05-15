#include "unity.h"
#include "bb_http_client.h"
#include "bb_http_client_host.h"
#include <string.h>
#include <stdlib.h>

static void reset(void)
{
    bb_http_client_clear_mock();
}

void test_bb_http_client_get_null_url_returns_invalid_arg(void)
{
    reset();
    char body[16];
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_client_get(NULL, body, sizeof(body), NULL, &r));
}

void test_bb_http_client_get_null_body_returns_invalid_arg(void)
{
    reset();
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_client_get("http://x", NULL, 0, NULL, &r));
}

void test_bb_http_client_get_null_out_returns_invalid_arg(void)
{
    reset();
    char body[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_client_get("http://x", body, sizeof(body), NULL, NULL));
}

void test_bb_http_client_get_zero_cap_returns_invalid_arg(void)
{
    reset();
    char body[16];
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_client_get("http://x", body, 0, NULL, &r));
}

void test_bb_http_client_get_no_mock_returns_invalid_state(void)
{
    reset();
    char body[16];
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_http_client_get("http://x", body, sizeof(body), NULL, &r));
}

void test_bb_http_client_get_mock_success_returns_body(void)
{
    reset();
    const char *payload = "{\"ok\":true}";
    bb_http_client_set_mock_response(payload, strlen(payload), 200);

    char body[64] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_get("http://example.com", body, sizeof(body), NULL, &r));
    TEST_ASSERT_EQUAL(200, r.status_code);
    TEST_ASSERT_EQUAL(strlen(payload), r.body_len);
    TEST_ASSERT_FALSE(r.truncated);
    TEST_ASSERT_EQUAL_STRING(payload, body);
}

void test_bb_http_client_get_mock_404_returns_ok_with_status(void)
{
    reset();
    bb_http_client_set_mock_response("not found", 9, 404);

    char body[64] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_get("http://example.com", body, sizeof(body), NULL, &r));
    TEST_ASSERT_EQUAL(404, r.status_code);
    TEST_ASSERT_EQUAL(9, r.body_len);
}

void test_bb_http_client_get_mock_truncates_when_body_too_big(void)
{
    reset();
    const char *payload = "0123456789ABCDEF";  // 16 bytes
    bb_http_client_set_mock_response(payload, strlen(payload), 200);

    char body[8] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_get("http://x", body, sizeof(body), NULL, &r));
    TEST_ASSERT_EQUAL(200, r.status_code);
    TEST_ASSERT_EQUAL(7, r.body_len);  // capped at body_cap-1
    TEST_ASSERT_TRUE(r.truncated);
    TEST_ASSERT_EQUAL_STRING_LEN("0123456", body, 7);
}

void test_bb_http_client_get_mock_transport_error_returns_passthrough(void)
{
    reset();
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    char body[16] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_http_client_get("http://x", body, sizeof(body), NULL, &r));
    TEST_ASSERT_EQUAL(0, r.status_code);
    TEST_ASSERT_EQUAL(0, r.body_len);
    TEST_ASSERT_FALSE(r.truncated);
}

void test_bb_http_client_get_empty_body_is_valid(void)
{
    reset();
    bb_http_client_set_mock_response("", 0, 204);
    char body[16] = {0xff};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_get("http://x", body, sizeof(body), NULL, &r));
    TEST_ASSERT_EQUAL(204, r.status_code);
    TEST_ASSERT_EQUAL(0, r.body_len);
    TEST_ASSERT_EQUAL('\0', body[0]);
}

void test_bb_http_client_get_cfg_honored(void)
{
    reset();
    bb_http_client_set_mock_response("hi", 2, 200);
    bb_http_client_cfg_t cfg = {
        .timeout_ms = 5000,
        .max_attempts = 1,
        .buffer_size = 1024,
        .user_agent = "test-agent/1.0",
        .accept_header = "application/json",
    };
    char body[16] = {0};
    bb_http_client_result_t r = {0};
    /* Mock ignores cfg but call must succeed with non-NULL cfg. */
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_get("http://x", body, sizeof(body), &cfg, &r));
    TEST_ASSERT_EQUAL(200, r.status_code);
}
