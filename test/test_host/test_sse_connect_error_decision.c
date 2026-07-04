#include "unity.h"
#include "sse_connect_error_decision.h"

// B1-561: pure bb_err_t -> HTTP status mapping for the events_handler()
// acquire path in bb_event_routes_espidf.c. A transient BB_ERR_NO_SPACE
// (heap-pressure allocation failure) must map to a retryable 503; any other
// error (genuine failure) stays a hard 500.

void test_sse_connect_error_status_no_space_returns_503(void)
{
    TEST_ASSERT_EQUAL(503, sse_connect_error_status(BB_ERR_NO_SPACE));
}

void test_sse_connect_error_status_other_returns_500(void)
{
    TEST_ASSERT_EQUAL(500, sse_connect_error_status(BB_ERR_INVALID_STATE));
}

void test_sse_connect_error_status_ok_returns_500(void)
{
    TEST_ASSERT_EQUAL(500, sse_connect_error_status(BB_OK));
}
