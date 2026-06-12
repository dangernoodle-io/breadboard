#include "unity.h"
#include "bb_http_client.h"
#include "bb_http_client_host.h"
#include <string.h>
#include <stdlib.h>

static void reset(void)
{
    bb_http_client_clear_mock();
}

// -------------------------------------------------------------------------
// NULL-arg guards
// -------------------------------------------------------------------------

void test_bb_http_client_post_null_url_returns_invalid_arg(void)
{
    reset();
    char resp[16];
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_post(NULL, "body", 4, NULL, resp, sizeof(resp), NULL, &r));
}

void test_bb_http_client_post_null_resp_returns_invalid_arg(void)
{
    reset();
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_post("http://x", "body", 4, NULL, NULL, 0, NULL, &r));
}

void test_bb_http_client_post_zero_resp_cap_returns_invalid_arg(void)
{
    reset();
    char resp[16];
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_post("http://x", "body", 4, NULL, resp, 0, NULL, &r));
}

void test_bb_http_client_post_null_out_returns_invalid_arg(void)
{
    reset();
    char resp[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_post("http://x", "body", 4, NULL, resp, sizeof(resp), NULL, NULL));
}

// -------------------------------------------------------------------------
// Transport error passthrough
// -------------------------------------------------------------------------

void test_bb_http_client_post_no_mock_returns_invalid_state(void)
{
    reset();
    char resp[16];
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
        bb_http_client_post("http://x", "body", 4, NULL, resp, sizeof(resp), NULL, &r));
}

void test_bb_http_client_post_transport_error_returns_passthrough(void)
{
    reset();
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);
    char resp[16] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
        bb_http_client_post("http://x", "body", 4, NULL, resp, sizeof(resp), NULL, &r));
    TEST_ASSERT_EQUAL(0, r.status_code);
    TEST_ASSERT_EQUAL(0, r.body_len);
    TEST_ASSERT_FALSE(r.truncated);
}

// -------------------------------------------------------------------------
// Success cases — method + url + body captured
// -------------------------------------------------------------------------

void test_bb_http_client_post_captures_method_url_body(void)
{
    reset();
    const char *payload = "{\"ok\":true}";
    bb_http_client_set_mock_response(payload, strlen(payload), 201);

    const char *req_body = "{\"data\":1}";
    char resp[64] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("http://example.com/api", req_body, strlen(req_body),
                            NULL, resp, sizeof(resp), NULL, &r));
    TEST_ASSERT_EQUAL(201, r.status_code);
    TEST_ASSERT_EQUAL(strlen(payload), r.body_len);
    TEST_ASSERT_FALSE(r.truncated);
    TEST_ASSERT_EQUAL_STRING(payload, resp);

    bb_http_client_post_record_t rec = bb_http_client_get_last_post();
    TEST_ASSERT_TRUE(rec.called);
    TEST_ASSERT_EQUAL_STRING("POST", rec.method);
    TEST_ASSERT_EQUAL_STRING("http://example.com/api", rec.url);
    TEST_ASSERT_EQUAL_PTR(req_body, rec.body);
    TEST_ASSERT_EQUAL(strlen(req_body), rec.body_len);
}

// -------------------------------------------------------------------------
// Content-Type default ("application/json") and override
// -------------------------------------------------------------------------

void test_bb_http_client_post_default_content_type_is_application_json(void)
{
    reset();
    bb_http_client_set_mock_response("ok", 2, 200);
    char resp[16] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("http://x", NULL, 0, NULL, resp, sizeof(resp), NULL, &r));

    bb_http_client_post_record_t rec = bb_http_client_get_last_post();
    TEST_ASSERT_EQUAL_STRING("application/json", rec.content_type);
}

void test_bb_http_client_post_content_type_override(void)
{
    reset();
    bb_http_client_set_mock_response("ok", 2, 200);
    char resp[16] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("http://x", NULL, 0, "text/plain",
                            resp, sizeof(resp), NULL, &r));

    bb_http_client_post_record_t rec = bb_http_client_get_last_post();
    TEST_ASSERT_EQUAL_STRING("text/plain", rec.content_type);
}

// -------------------------------------------------------------------------
// TLS fields threaded through cfg
// -------------------------------------------------------------------------

void test_bb_http_client_post_no_cfg_no_tls_fields(void)
{
    reset();
    bb_http_client_set_mock_response("ok", 2, 200);
    char resp[16] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("http://x", NULL, 0, NULL, resp, sizeof(resp), NULL, &r));

    bb_http_client_post_record_t rec = bb_http_client_get_last_post();
    TEST_ASSERT_FALSE(rec.has_ca_cert);
    TEST_ASSERT_FALSE(rec.has_client_cert);
    TEST_ASSERT_FALSE(rec.has_client_key);
}

void test_bb_http_client_post_cfg_with_client_cert_key_recorded(void)
{
    reset();
    bb_http_client_set_mock_response("{}", 2, 200);
    bb_http_client_cfg_t cfg = {
        .ca_cert_pem     = "-----BEGIN CERTIFICATE-----\nCA\n-----END CERTIFICATE-----\n",
        .client_cert_pem = "-----BEGIN CERTIFICATE-----\nCLIENT\n-----END CERTIFICATE-----\n",
        .client_key_pem  = "-----BEGIN PRIVATE KEY-----\nKEY\n-----END PRIVATE KEY-----\n",
    };
    char resp[16] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("https://secure.example.com/pub", "data", 4, NULL,
                            resp, sizeof(resp), &cfg, &r));

    bb_http_client_post_record_t rec = bb_http_client_get_last_post();
    TEST_ASSERT_TRUE(rec.has_ca_cert);
    TEST_ASSERT_TRUE(rec.has_client_cert);
    TEST_ASSERT_TRUE(rec.has_client_key);
}

void test_bb_http_client_post_cfg_partial_no_client_key(void)
{
    reset();
    bb_http_client_set_mock_response("{}", 2, 200);
    bb_http_client_cfg_t cfg = {
        .client_cert_pem = "-----BEGIN CERTIFICATE-----\nCLIENT\n-----END CERTIFICATE-----\n",
        // client_key_pem intentionally NULL
    };
    char resp[16] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("http://x", NULL, 0, NULL, resp, sizeof(resp), &cfg, &r));

    bb_http_client_post_record_t rec = bb_http_client_get_last_post();
    TEST_ASSERT_TRUE(rec.has_client_cert);
    TEST_ASSERT_FALSE(rec.has_client_key);
    TEST_ASSERT_FALSE(rec.has_ca_cert);
}

// -------------------------------------------------------------------------
// Response status + body returned; truncation
// -------------------------------------------------------------------------

void test_bb_http_client_post_404_status_returned(void)
{
    reset();
    bb_http_client_set_mock_response("not found", 9, 404);
    char resp[64] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("http://x", NULL, 0, NULL, resp, sizeof(resp), NULL, &r));
    TEST_ASSERT_EQUAL(404, r.status_code);
    TEST_ASSERT_EQUAL(9, r.body_len);
    TEST_ASSERT_FALSE(r.truncated);
}

void test_bb_http_client_post_response_truncated(void)
{
    reset();
    const char *payload = "0123456789ABCDEF";  // 16 bytes
    bb_http_client_set_mock_response(payload, strlen(payload), 200);
    char resp[8] = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("http://x", NULL, 0, NULL, resp, sizeof(resp), NULL, &r));
    TEST_ASSERT_EQUAL(200, r.status_code);
    TEST_ASSERT_EQUAL(7, r.body_len);
    TEST_ASSERT_TRUE(r.truncated);
}

void test_bb_http_client_post_empty_response_body(void)
{
    reset();
    bb_http_client_set_mock_response("", 0, 204);
    char resp[16] = {0xff};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_post("http://x", NULL, 0, NULL, resp, sizeof(resp), NULL, &r));
    TEST_ASSERT_EQUAL(204, r.status_code);
    TEST_ASSERT_EQUAL(0, r.body_len);
    TEST_ASSERT_EQUAL('\0', resp[0]);
}

// -------------------------------------------------------------------------
// record cleared by clear_mock
// -------------------------------------------------------------------------

void test_bb_http_client_post_clear_mock_resets_record(void)
{
    reset();
    bb_http_client_set_mock_response("ok", 2, 200);
    char resp[16] = {0};
    bb_http_client_result_t r = {0};
    bb_http_client_post("http://x", NULL, 0, NULL, resp, sizeof(resp), NULL, &r);

    bb_http_client_post_record_t rec = bb_http_client_get_last_post();
    TEST_ASSERT_TRUE(rec.called);

    bb_http_client_clear_mock();
    rec = bb_http_client_get_last_post();
    TEST_ASSERT_FALSE(rec.called);
}
