// Tests for bb_http_client session API (keep-alive session):
// - session_open NULL-arg guards
// - session_post NULL-arg guards
// - session_close NULL safe
// - session_post records url/body/content_type
// - session_post default content-type application/json
// - session_post transport error passthrough
// - session_post 2xx returns BB_OK; non-2xx returns BB_ERR_INVALID_STATE
// - clear_mock resets session record
#include "unity.h"
#include "bb_http_client.h"
#include "../../platform/host/bb_http_client/bb_http_client_host.h"
#include <string.h>
#include <stdlib.h>

static void reset(void)
{
    bb_http_client_clear_mock();
}

// -------------------------------------------------------------------------
// session_open NULL-arg guards
// -------------------------------------------------------------------------

void test_bb_http_client_session_open_null_url_base_returns_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_open(NULL, NULL, &s));
}

void test_bb_http_client_session_open_null_out_returns_invalid_arg(void)
{
    reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_open(NULL, "http://x", NULL));
}

void test_bb_http_client_session_open_succeeds(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));
    TEST_ASSERT_NOT_NULL(s);
    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// session_post NULL-arg guards
// -------------------------------------------------------------------------

void test_bb_http_client_session_post_null_session_returns_invalid_arg(void)
{
    reset();
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_post(NULL, "http://x", "{}", 2, NULL, &r));
}

void test_bb_http_client_session_post_null_url_returns_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_post(s, NULL, "{}", 2, NULL, &r));
    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_null_out_returns_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_post(s, "http://x", "{}", 2, NULL, NULL));
    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// session_close NULL safe
// -------------------------------------------------------------------------

void test_bb_http_client_session_close_null_is_safe(void)
{
    bb_http_client_session_close(NULL);  // must not crash
}

// -------------------------------------------------------------------------
// session_post records captured values
// -------------------------------------------------------------------------

void test_bb_http_client_session_post_captures_url_body_content_type(void)
{
    reset();
    bb_http_client_session_set_mock_status(200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    const char *body = "{\"ok\":true}";
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub",
                                    body, strlen(body),
                                    "application/json", &r));
    TEST_ASSERT_EQUAL(200, r.status_code);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);
    TEST_ASSERT_EQUAL_STRING("http://example.com/pub", rec.url);
    TEST_ASSERT_EQUAL_PTR(body, rec.body);
    TEST_ASSERT_EQUAL(strlen(body), rec.body_len);
    TEST_ASSERT_EQUAL_STRING("application/json", rec.content_type);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_default_content_type_application_json(void)
{
    reset();
    bb_http_client_session_set_mock_status(200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub",
                                    NULL, 0, NULL, &r));

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_EQUAL_STRING("application/json", rec.content_type);

    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// Transport error passthrough
// -------------------------------------------------------------------------

void test_bb_http_client_session_post_transport_error_passthrough(void)
{
    reset();
    bb_http_client_session_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
        bb_http_client_session_post(s, "http://example.com/pub",
                                    "{}", 2, NULL, &r));
    TEST_ASSERT_EQUAL(0, r.status_code);

    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// 2xx → BB_OK; non-2xx → BB_ERR_INVALID_STATE
// -------------------------------------------------------------------------

void test_bb_http_client_session_post_2xx_returns_ok(void)
{
    reset();
    bb_http_client_session_set_mock_status(201);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub",
                                    "{}", 2, NULL, &r));
    TEST_ASSERT_EQUAL(201, r.status_code);

    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// clear_mock resets session record
// -------------------------------------------------------------------------

void test_bb_http_client_session_clear_mock_resets_record(void)
{
    reset();
    bb_http_client_session_set_mock_status(200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r = {0};
    bb_http_client_session_post(s, "http://example.com/pub", "{}", 2, NULL, &r);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);

    bb_http_client_clear_mock();
    rec = bb_http_client_session_last_post();
    TEST_ASSERT_FALSE(rec.called);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_tls_error_code_propagated(void)
{
    bb_http_client_session_t sess;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "https://example.com", &sess));
    bb_http_client_session_set_mock_tls_error_code(-0x7200);
    bb_http_client_session_set_mock_status(200);
    bb_http_client_result_t res;
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_post(sess, "https://example.com/path", NULL, 0, NULL, &res));
    TEST_ASSERT_EQUAL_INT(-0x7200, res.tls_error_code);
    bb_http_client_session_close(sess);
    bb_http_client_clear_mock();
}

void test_bb_http_client_session_post_tls_error_code_zero_default(void)
{
    bb_http_client_session_t sess;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "https://example.com", &sess));
    // No tls mock set — default is 0.
    bb_http_client_session_set_mock_status(200);
    bb_http_client_result_t res;
    memset(&res, 0xff, sizeof(res));  // Fill with non-zero to detect if 0 is set
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_post(sess, "https://example.com/path", NULL, 0, NULL, &res));
    TEST_ASSERT_EQUAL_INT(0, res.tls_error_code);
    bb_http_client_session_close(sess);
    bb_http_client_clear_mock();
}
