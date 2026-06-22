#include "unity.h"
#include "bb_http_status.h"

void test_bb_http_status_reason_known_codes(void)
{
    // Every case branch in the table must be exercised for full coverage.
    TEST_ASSERT_EQUAL_STRING("200 OK", bb_http_status_reason(200));
    TEST_ASSERT_EQUAL_STRING("201 Created", bb_http_status_reason(201));
    TEST_ASSERT_EQUAL_STRING("202 Accepted", bb_http_status_reason(202));
    TEST_ASSERT_EQUAL_STRING("204 No Content", bb_http_status_reason(204));
    TEST_ASSERT_EQUAL_STRING("302 Found", bb_http_status_reason(302));
    TEST_ASSERT_EQUAL_STRING("400 Bad Request", bb_http_status_reason(400));
    TEST_ASSERT_EQUAL_STRING("401 Unauthorized", bb_http_status_reason(401));
    TEST_ASSERT_EQUAL_STRING("403 Forbidden", bb_http_status_reason(403));
    TEST_ASSERT_EQUAL_STRING("404 Not Found", bb_http_status_reason(404));
    TEST_ASSERT_EQUAL_STRING("408 Request Timeout", bb_http_status_reason(408));
    TEST_ASSERT_EQUAL_STRING("409 Conflict", bb_http_status_reason(409));
    TEST_ASSERT_EQUAL_STRING("412 Precondition Failed", bb_http_status_reason(412));
    TEST_ASSERT_EQUAL_STRING("422 Unprocessable Entity", bb_http_status_reason(422));
    TEST_ASSERT_EQUAL_STRING("500 Internal Server Error", bb_http_status_reason(500));
    TEST_ASSERT_EQUAL_STRING("503 Service Unavailable", bb_http_status_reason(503));
}

void test_bb_http_status_reason_unknown_returns_null(void)
{
    TEST_ASSERT_NULL(bb_http_status_reason(0));
    TEST_ASSERT_NULL(bb_http_status_reason(418));
    TEST_ASSERT_NULL(bb_http_status_reason(599));
}
