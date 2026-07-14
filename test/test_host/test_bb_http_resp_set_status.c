// B1-954: bb_http_resp_set_status must never leave the emitted status
// unset/silent, whether or not the code is in the bb_http_status_reason
// table. These assert the *emitted status* (via the host capture harness),
// not the return value of bb_http_resp_set_status — the return value alone
// missed the fail-silent bug for 501 (PR #868) and would have missed it for
// 413 here too.
#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"

void test_resp_set_status_413_emits_413(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);

    bb_err_t err = bb_http_resp_set_status(req, 413);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL_INT(413, cap.status);
    bb_http_host_capture_free(&cap);
}

// The class this closes: a status code that is NOT in the SSOT table must
// still be emitted correctly (the number is what an HTTP client acts on),
// not silently dropped in favour of whatever status was set/defaulted
// before. 429 is deliberately not in bb_http_status.c's table.
void test_resp_set_status_untabled_code_still_emits_that_code(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);

    bb_err_t err = bb_http_resp_set_status(req, 429);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL_INT(429, cap.status);
    bb_http_host_capture_free(&cap);
}

// Regression guard (B1-954): 501 must keep emitting 501, not fall through to
// the generic untabled fallback (which would still be numerically correct,
// but the regression this guards against — dropping 501 unnoticed from the
// table — is exactly the class of bug this fix closes).
void test_resp_set_status_501_still_tabled(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);

    bb_err_t err = bb_http_resp_set_status(req, 501);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL_INT(501, cap.status);
    bb_http_host_capture_free(&cap);
}
