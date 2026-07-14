#include "unity.h"
#include "bb_http_status.h"
#include <limits.h>
#include <string.h>

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
    TEST_ASSERT_EQUAL_STRING("413 Payload Too Large", bb_http_status_reason(413));
    TEST_ASSERT_EQUAL_STRING("422 Unprocessable Entity", bb_http_status_reason(422));
    TEST_ASSERT_EQUAL_STRING("500 Internal Server Error", bb_http_status_reason(500));
    // Regression guard (B1-954): 501 shipped to the espidf switch but not the
    // host mirror before this SSOT table existed (PR #868); a table edit that
    // drops it must fail here, not silently ship a wrong status again.
    TEST_ASSERT_EQUAL_STRING("501 Not Implemented", bb_http_status_reason(501));
    TEST_ASSERT_EQUAL_STRING("503 Service Unavailable", bb_http_status_reason(503));
}

void test_bb_http_status_reason_unknown_returns_null(void)
{
    TEST_ASSERT_NULL(bb_http_status_reason(0));
    TEST_ASSERT_NULL(bb_http_status_reason(418));
    TEST_ASSERT_NULL(bb_http_status_reason(599));
}

// ============================================================================
// bb_http_status_line — the non-silent fallback (B1-954)
// ============================================================================

void test_bb_http_status_line_known_code_matches_reason(void)
{
    char buf[24];
    // A tabled code returns the same static literal as bb_http_status_reason
    // — the fallback_buf is untouched (not written) for the known-code path.
    memset(buf, 0xAA, sizeof(buf));
    const char *line = bb_http_status_line(413, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("413 Payload Too Large", line);
    TEST_ASSERT_EQUAL_PTR(bb_http_status_reason(413), line);
}

void test_bb_http_status_line_untabled_code_emits_correct_number(void)
{
    // The exact defect class this closes: an untabled code (e.g. a future
    // 429) must never leave the status unset. The number is what a client
    // acts on; the phrase is a generic, clearly-marked fallback.
    char buf[24];
    const char *line = bb_http_status_line(429, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_EQUAL_STRING("429 Unknown Status", line);
    TEST_ASSERT_EQUAL_PTR(buf, line);
}

void test_bb_http_status_line_untabled_null_buf_returns_null(void)
{
    TEST_ASSERT_NULL(bb_http_status_line(429, NULL, 24));
}

void test_bb_http_status_line_untabled_zero_len_buf_returns_null(void)
{
    char buf[24];
    TEST_ASSERT_NULL(bb_http_status_line(429, buf, 0));
}

// A code outside [100, 599] is not a valid HTTP status at all — must be
// rejected outright, not formatted (formatting a pathological value like
// INT_MIN would silently truncate into the 24-byte buffer while still
// returning non-NULL, i.e. reporting success on a corrupted line).
void test_bb_http_status_line_out_of_range_code_returns_null(void)
{
    char buf[24];
    memset(buf, 0xAA, sizeof(buf));
    TEST_ASSERT_NULL(bb_http_status_line(99, buf, sizeof(buf)));
    TEST_ASSERT_NULL(bb_http_status_line(600, buf, sizeof(buf)));
    TEST_ASSERT_NULL(bb_http_status_line(INT_MIN, buf, sizeof(buf)));
    TEST_ASSERT_NULL(bb_http_status_line(INT_MAX, buf, sizeof(buf)));
}
