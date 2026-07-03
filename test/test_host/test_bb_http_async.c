// Host stub contract tests for bb_http's async-handler / peer-liveness API
// (bb_http_req_async_handler_begin/complete/abort, bb_http_req_peer_alive).
// The host backend has no real httpd socket, so these functions are fixed
// stubs — this guards against a future accidental stub flip (B1-517 review).
#include "unity.h"
#include "bb_http.h"

void test_bb_http_req_peer_alive_host_stub_always_true(void)
{
    // Host has no real socket to probe; the stub always reports alive.
    TEST_ASSERT_TRUE(bb_http_req_peer_alive(NULL));
}

void test_bb_http_req_async_abort_host_stub_returns_invalid_state(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, bb_http_req_async_abort(NULL));
}

void test_bb_http_req_async_handler_begin_host_stub_returns_invalid_state(void)
{
    bb_http_request_t *out = NULL;
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE,
                          bb_http_req_async_handler_begin(NULL, &out));
    TEST_ASSERT_NULL(out);
}

void test_bb_http_req_async_handler_complete_host_stub_returns_invalid_state(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE,
                          bb_http_req_async_handler_complete(NULL));
}
