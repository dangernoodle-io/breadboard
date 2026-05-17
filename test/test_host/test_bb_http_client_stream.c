// Tests for bb_http_client_get_stream on the host mock backend.
//
// The host port replays the mock body in ~256-byte chunks via the callback.
// These tests verify: argument validation, transport-error pass-through,
// callback invocation, and early-stop (BB_ERR_NO_SPACE) propagation.

#include "unity.h"
#include "bb_http_client.h"
#include "bb_http_client_host.h"
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Simple accumulator callback — collects all chunks into a buffer.
// ---------------------------------------------------------------------------

typedef struct {
    char   buf[1024];
    size_t len;
    int    call_count;
} accum_ctx_t;

static bb_err_t accum_cb(void *cv, const char *data, size_t n)
{
    accum_ctx_t *ac = (accum_ctx_t *)cv;
    ac->call_count++;
    if (ac->len + n < sizeof(ac->buf)) {
        memcpy(ac->buf + ac->len, data, n);
        ac->len += n;
        ac->buf[ac->len] = '\0';
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Early-stop callback — aborts after `limit` total bytes.
// ---------------------------------------------------------------------------

typedef struct {
    size_t limit;
    size_t received;
    bool   stopped;
} stop_ctx_t;

static bb_err_t stop_cb(void *cv, const char *data, size_t n)
{
    stop_ctx_t *sc = (stop_ctx_t *)cv;
    sc->received += n;
    if (sc->received >= sc->limit) {
        sc->stopped = true;
        return BB_ERR_NO_SPACE;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Error-propagation callback — always returns a custom error.
// ---------------------------------------------------------------------------

static bb_err_t error_cb(void *cv, const char *data, size_t n)
{
    (void)cv; (void)data; (void)n;
    return BB_ERR_NOT_FOUND;
}

static void reset(void)
{
    bb_http_client_clear_mock();
}

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_null_url_returns_invalid_arg(void)
{
    reset();
    accum_ctx_t ac = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_get_stream(NULL, accum_cb, &ac, NULL, &r));
}

void test_bb_http_client_stream_null_cb_returns_invalid_arg(void)
{
    reset();
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_get_stream("http://x", NULL, NULL, NULL, &r));
}

void test_bb_http_client_stream_null_out_returns_invalid_arg(void)
{
    reset();
    accum_ctx_t ac = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_get_stream("http://x", accum_cb, &ac, NULL, NULL));
}

// ---------------------------------------------------------------------------
// Transport error propagated before any cb() calls
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_transport_error_propagated(void)
{
    reset();
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    accum_ctx_t ac = {0};
    bb_http_client_result_t r = {0};
    bb_err_t err = bb_http_client_get_stream("http://x", accum_cb, &ac, NULL, &r);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(0, ac.call_count);
    TEST_ASSERT_EQUAL(0, r.body_len);
    TEST_ASSERT_EQUAL(0, r.status_code);
    TEST_ASSERT_FALSE(r.truncated);
}

// ---------------------------------------------------------------------------
// Success: body reassembled across chunks equals original payload
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_small_body_reassembled(void)
{
    reset();
    const char *payload = "{\"ok\":true}";
    bb_http_client_set_mock_response(payload, strlen(payload), 200);

    accum_ctx_t ac = {0};
    bb_http_client_result_t r = {0};
    bb_err_t err = bb_http_client_get_stream("http://x", accum_cb, &ac, NULL, &r);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(200, r.status_code);
    TEST_ASSERT_EQUAL(strlen(payload), r.body_len);
    TEST_ASSERT_FALSE(r.truncated);
    TEST_ASSERT_EQUAL(strlen(payload), ac.len);
    TEST_ASSERT_EQUAL_STRING(payload, ac.buf);
    TEST_ASSERT_GREATER_OR_EQUAL(1, ac.call_count);
}

// ---------------------------------------------------------------------------
// Large body: >256 bytes -> multiple chunks
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_large_body_multiple_chunks(void)
{
    reset();
    // 600-byte payload -> at least 3 chunks from the host mock (256-byte chunks)
    char payload[601];
    memset(payload, 'A', 600);
    payload[600] = '\0';
    bb_http_client_set_mock_response(payload, 600, 200);

    accum_ctx_t ac = {0};
    bb_http_client_result_t r = {0};
    bb_err_t err = bb_http_client_get_stream("http://x", accum_cb, &ac, NULL, &r);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(200, r.status_code);
    TEST_ASSERT_EQUAL(600, (int)r.body_len);
    TEST_ASSERT_EQUAL(600, (int)ac.len);
    TEST_ASSERT_GREATER_OR_EQUAL(3, ac.call_count);
}

// ---------------------------------------------------------------------------
// Empty body: cb not called, body_len = 0
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_empty_body(void)
{
    reset();
    bb_http_client_set_mock_response("", 0, 204);

    accum_ctx_t ac = {0};
    bb_http_client_result_t r = {0};
    bb_err_t err = bb_http_client_get_stream("http://x", accum_cb, &ac, NULL, &r);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(204, r.status_code);
    TEST_ASSERT_EQUAL(0, r.body_len);
    TEST_ASSERT_EQUAL(0, ac.call_count);
}

// ---------------------------------------------------------------------------
// Early stop: BB_ERR_NO_SPACE from cb -> out->truncated = true
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_early_stop_sets_truncated(void)
{
    reset();
    char payload[600];
    memset(payload, 'B', 599);
    payload[599] = '\0';
    bb_http_client_set_mock_response(payload, 599, 200);

    stop_ctx_t sc = { .limit = 100, .received = 0, .stopped = false };
    bb_http_client_result_t r = {0};
    bb_err_t err = bb_http_client_get_stream("http://x", stop_cb, &sc, NULL, &r);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_TRUE(r.truncated);
    TEST_ASSERT_TRUE(sc.stopped);
}

// ---------------------------------------------------------------------------
// cb error other than NO_SPACE propagated as-is, truncated = false
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_cb_error_propagated(void)
{
    reset();
    const char *payload = "hello";
    bb_http_client_set_mock_response(payload, strlen(payload), 200);

    bb_http_client_result_t r = {0};
    bb_err_t err = bb_http_client_get_stream("http://x", error_cb, NULL, NULL, &r);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
    TEST_ASSERT_FALSE(r.truncated);
}

// ---------------------------------------------------------------------------
// 404 response: cb still called with body, status_code = 404
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_404_status_code(void)
{
    reset();
    const char *body = "Not Found";
    bb_http_client_set_mock_response(body, strlen(body), 404);

    accum_ctx_t ac = {0};
    bb_http_client_result_t r = {0};
    bb_err_t err = bb_http_client_get_stream("http://x", accum_cb, &ac, NULL, &r);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, r.status_code);
    TEST_ASSERT_EQUAL(strlen(body), r.body_len);
}

// ---------------------------------------------------------------------------
// cfg arg is accepted (ignored by host mock, but must not crash)
// ---------------------------------------------------------------------------

void test_bb_http_client_stream_cfg_honored(void)
{
    reset();
    bb_http_client_set_mock_response("ok", 2, 200);
    bb_http_client_cfg_t cfg = {
        .timeout_ms = 5000,
        .max_attempts = 1,
        .buffer_size = 1024,
        .user_agent = "test-agent/1.0",
        .accept_header = "application/json",
    };
    accum_ctx_t ac = {0};
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_get_stream("http://x", accum_cb, &ac, &cfg, &r));
    TEST_ASSERT_EQUAL(200, r.status_code);
}
