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
#include "bb_http_server.h"
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
// bb_http_req_recv_body_stack (bb_http_section PR review, MEDIUM finding).
//
// Extracts the validate-cap -> fixed-stack-buffer -> recv idiom
// bb_http_section_dispatch.c, bb_wifi_http_routes.c's wifi_patch_handler,
// and bb_storage_http_routes.c's factory_reset_handler each hand-rolled
// independently -- every one of those hand-rolled copies checked
// `body_len > buf_cap` but then called
// bb_http_req_recv(req, buf, sizeof(buf) - 1), silently truncating a body
// of EXACTLY buf_cap bytes by one byte. test_bb_http_body_recv_body_stack_
// exactly_at_cap_not_truncated below is the off-by-one regression pin: it
// goes RED if that bug is reintroduced (e.g. by changing the helper's
// internal recv() call back to `buf_cap` instead of `max_body`).
// ---------------------------------------------------------------------------

void test_bb_http_body_recv_body_stack_happy_path(void)
{
    const char *payload = "{\"key\":\"value\"}";
    bb_http_request_t *req = begin_with_body(payload);

    char   buf[64];
    size_t len = 0;
    bb_err_t rc = bb_http_req_recv_body_stack(req, buf, sizeof(buf), &len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT((unsigned)strlen(payload), (unsigned)len);
    TEST_ASSERT_EQUAL_STRING(payload, buf);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[len]);

    end_cap(req);
}

// REVERT-PROOF (the off-by-one): a body of EXACTLY buf_cap - 1 bytes (the
// max this cap can hold, per the cap-includes-NUL-slot semantics) must be
// read in FULL, not truncated by one byte. If the helper's internal
// bb_http_req_recv() call were changed from `max_body` (buf_cap - 1) back
// to the old hand-rolled `buf_cap` (or, equivalently, if the pre-check were
// loosened to allow body_len == buf_cap), this test goes RED: `len` would
// be one byte short and the last character of `payload` would be missing
// from `buf`.
void test_bb_http_body_recv_body_stack_exactly_at_cap_not_truncated(void)
{
    #define STACK_TEST_BUF_CAP 8  // max body = 7 bytes
    char payload[STACK_TEST_BUF_CAP];  // 7 bytes + NUL, exactly at cap
    memset(payload, 'A', STACK_TEST_BUF_CAP - 1);
    payload[STACK_TEST_BUF_CAP - 1] = '\0';
    TEST_ASSERT_EQUAL(STACK_TEST_BUF_CAP - 1, (int)strlen(payload));

    bb_http_request_t *req = begin_with_body(payload);

    char   buf[STACK_TEST_BUF_CAP];
    size_t len = 0;
    bb_err_t rc = bb_http_req_recv_body_stack(req, buf, sizeof(buf), &len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(STACK_TEST_BUF_CAP - 1, (unsigned)len);
    TEST_ASSERT_EQUAL_STRING(payload, buf);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[len]);
    #undef STACK_TEST_BUF_CAP

    end_cap(req);
}

void test_bb_http_body_recv_body_stack_over_cap_returns_invalid_arg(void)
{
    const char *payload = "ABCDEFGHIJ";  // 10 bytes
    bb_http_request_t *req = begin_with_body(payload);

    char   buf[5];  // max body = 4 bytes; payload is 10 -> rejected
    size_t len = 0;
    bb_err_t rc = bb_http_req_recv_body_stack(req, buf, sizeof(buf), &len);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);

    end_cap(req);
}

void test_bb_http_body_recv_body_stack_zero_len_returns_invalid_arg(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    char   buf[64];
    size_t len = 0;
    bb_err_t rc = bb_http_req_recv_body_stack(req, buf, sizeof(buf), &len);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);

    end_cap(req);
}

void test_bb_http_body_recv_body_stack_zero_cap_returns_invalid_arg(void)
{
    bb_http_request_t *req = begin_with_body("x");

    char   buf[1];
    size_t len = 0;
    bb_err_t rc = bb_http_req_recv_body_stack(req, buf, 0, &len);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);

    end_cap(req);
}

void test_bb_http_body_recv_body_stack_recv_fail_returns_invalid_arg(void)
{
    const char *payload = "{\"x\":1}";
    bb_http_request_t *req = begin_with_body(payload);

    bb_http_host_force_recv_fail(true);

    char   buf[64];
    size_t len = 0;
    bb_err_t rc = bb_http_req_recv_body_stack(req, buf, sizeof(buf), &len);

    bb_http_host_force_recv_fail(false);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);

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
