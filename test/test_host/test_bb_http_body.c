// Tests for bb_http_req_recv_body_alloc (B1-335).
//
// Covers all branches of the helper:
//   1. Happy path: body allocated, NUL-terminated, correct length, BB_OK.
//   2. Zero-length body (body_len <= 0): BB_ERR_INVALID_ARG, no allocation.
//   3. Body exceeds max_bytes: BB_ERR_NO_SPACE, no allocation.
//   4. recv failure: BB_ERR_INVALID_ARG, no allocation (helper frees on recv fail).
//   5. OOM (malloc failure via BB_HTTP_BODY_TESTING hook): BB_ERR_NO_SPACE.

#include "unity.h"
#include "bb_http_body.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "test_alloc_inject.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Helper: begin a capture with an optional body
// ---------------------------------------------------------------------------

static bb_http_request_t *begin_with_body(const char *body)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (body) {
        bb_http_host_capture_set_req_body(body, (int)strlen(body));
    }
    return req;
}

static void end_cap(bb_http_request_t *req)
{
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// 1. Happy path
// ---------------------------------------------------------------------------

void test_bb_http_body_happy_path(void)
{
    const char *payload = "{\"key\":\"value\"}";
    bb_http_request_t *req = begin_with_body(payload);

    char *buf = NULL;
    int   len = 0;
    bb_err_t rc = bb_http_req_recv_body_alloc(req, 1024, &buf, &len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(payload), len);
    TEST_ASSERT_EQUAL_STRING(payload, buf);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[len]);

    free(buf);
    end_cap(req);
}

// ---------------------------------------------------------------------------
// 2. Zero-length body (body_len == 0)
// ---------------------------------------------------------------------------

void test_bb_http_body_zero_len_returns_invalid_arg(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    char *buf = NULL;
    int   len = 0;
    bb_err_t rc = bb_http_req_recv_body_alloc(req, 1024, &buf, &len);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
    TEST_ASSERT_NULL(buf);

    end_cap(req);
}

// ---------------------------------------------------------------------------
// 3. Body exceeds max_bytes
// ---------------------------------------------------------------------------

void test_bb_http_body_over_max_returns_no_space(void)
{
    const char *payload = "ABCDEFGHIJ";  // 10 bytes
    bb_http_request_t *req = begin_with_body(payload);

    char *buf = NULL;
    int   len = 0;
    bb_err_t rc = bb_http_req_recv_body_alloc(req, 5, &buf, &len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(buf);

    end_cap(req);
}

// ---------------------------------------------------------------------------
// 4. recv failure
// ---------------------------------------------------------------------------

void test_bb_http_body_recv_fail_returns_invalid_arg(void)
{
    const char *payload = "{\"x\":1}";
    bb_http_request_t *req = begin_with_body(payload);

    bb_http_host_force_recv_fail(true);

    char *buf = NULL;
    int   len = 0;
    bb_err_t rc = bb_http_req_recv_body_alloc(req, 1024, &buf, &len);

    bb_http_host_force_recv_fail(false);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
    TEST_ASSERT_NULL(buf);

    end_cap(req);
}

// ---------------------------------------------------------------------------
// 5. OOM — swap malloc via BB_HTTP_BODY_TESTING hook
// ---------------------------------------------------------------------------

static void *s_fail_malloc(size_t sz)
{
    (void)sz;
    return NULL;
}

void test_bb_http_body_oom_returns_no_space(void)
{
    const char *payload = "{\"y\":2}";
    bb_http_request_t *req = begin_with_body(payload);

    bb_http_body_set_malloc(s_fail_malloc);

    char *buf = NULL;
    int   len = 0;
    bb_err_t rc = bb_http_req_recv_body_alloc(req, 1024, &buf, &len);

    bb_http_body_set_malloc(NULL);  // restore libc malloc

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(buf);

    end_cap(req);
}

// ---------------------------------------------------------------------------
// bb_http_req_get_header — host test-hook-backed request header accessor.
// ---------------------------------------------------------------------------

void test_bb_http_req_get_header_found(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_set_req_header("User-Agent", "curl/8.0");

    char out[64] = {0};
    bb_err_t rc = bb_http_req_get_header(req, "User-Agent", out, sizeof(out));

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("curl/8.0", out);

    bb_http_host_set_req_header(NULL, NULL);
    end_cap(req);
}

void test_bb_http_req_get_header_not_found(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_set_req_header(NULL, NULL);

    char out[64] = {0};
    bb_err_t rc = bb_http_req_get_header(req, "User-Agent", out, sizeof(out));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, rc);

    end_cap(req);
}

void test_bb_http_req_get_header_name_set_value_null_not_found(void)
{
    // Exercises the s_req_header_value==NULL sub-branch independently of
    // s_req_header_name==NULL (distinct from the both-null case above).
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_set_req_header("User-Agent", NULL);

    char out[64] = {0};
    bb_err_t rc = bb_http_req_get_header(req, "User-Agent", out, sizeof(out));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, rc);

    bb_http_host_set_req_header(NULL, NULL);
    end_cap(req);
}

void test_bb_http_req_get_header_wrong_name_not_found(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_set_req_header("X-Client-Id", "board-1");

    char out[64] = {0};
    bb_err_t rc = bb_http_req_get_header(req, "User-Agent", out, sizeof(out));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, rc);

    bb_http_host_set_req_header(NULL, NULL);
    end_cap(req);
}

void test_bb_http_req_get_header_null_args(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_set_req_header("User-Agent", "curl/8.0");

    char out[64] = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_req_get_header(req, NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_req_get_header(req, "User-Agent", NULL, sizeof(out)));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_req_get_header(req, "User-Agent", out, 0));

    bb_http_host_set_req_header(NULL, NULL);
    end_cap(req);
}

void test_bb_http_req_get_header_truncates_to_out_len(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_set_req_header("User-Agent", "a-very-long-user-agent-string");

    char out[6] = {0};
    bb_err_t rc = bb_http_req_get_header(req, "User-Agent", out, sizeof(out));

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("a-ver", out);  // 5 chars + NUL

    bb_http_host_set_req_header(NULL, NULL);
    end_cap(req);
}
