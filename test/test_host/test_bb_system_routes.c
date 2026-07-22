// Tests for the POST /api/reboot route handler (B1-1148 PR1).
//
// PR1 is build-wiring + restart gating ONLY -- reboot_handler still calls
// bb_system_reboot_parse_body() exactly as it did before (that pure fn's
// own parse-logic branches are already covered by test_bb_system.c). This
// file covers the HTTP-handler seam itself: body plumbing, the User-Agent
// header fallback, and that the handler always responds 200 (malformed
// input is tolerated, not rejected -- current behavior, unchanged by PR1).
//
// The device-only restart call (bb_system_restart_reason_at(), whose host
// stub calls exit(0)) is compiled out on host via #ifdef ESP_PLATFORM in
// bb_system_routes.c -- these tests never reach it, by construction.
//
// Because that call is the only consumer of the resolved (detail, ts) pair,
// and the HTTP response body never carries them, the User-Agent-fallback /
// "what actually got resolved" tests below assert via a dedicated
// BB_SYSTEM_TESTING capture seam (bb_system_reboot_capture_get_for_test())
// that records the resolved values immediately before the restart call --
// same shape as bb_storage_http's s_erase_all_calls fixture (see
// test_bb_storage_http_factory_reset.c). This makes them load-bearing: a
// fallback/plumbing regression turns them red, not just a status-code check
// that any of these tests would already pass. Reset in setUp() via
// bb_system_reboot_capture_reset_for_test() (test_main.c).

#include "unity.h"
#include "bb_system.h"
#include "bb_system_test.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helper: run the reboot handler with an optional JSON body / User-Agent
// header, returning the captured response.
// ---------------------------------------------------------------------------

static bb_http_host_capture_t run_reboot(const char *body, const char *user_agent)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (body) {
        bb_http_host_capture_set_req_body(body, (int)strlen(body));
    }
    bb_http_host_set_req_header("User-Agent", user_agent);
    bb_system_reboot_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    bb_http_host_set_req_header("User-Agent", NULL);
    return cap;
}

// ---------------------------------------------------------------------------
// Absent / empty body — no ts, no detail, still 200
// ---------------------------------------------------------------------------

void test_bb_system_reboot_route_no_body_returns_200(void)
{
    bb_http_host_capture_t cap = run_reboot(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);

    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(j, "200 body is not valid JSON");
    cJSON *status = cJSON_GetObjectItemCaseSensitive(j, "status");
    TEST_ASSERT_NOT_NULL(status);
    TEST_ASSERT_TRUE(cJSON_IsString(status));
    TEST_ASSERT_EQUAL_STRING("rebooting", cJSON_GetStringValue(status));
    cJSON_Delete(j);

    bb_http_host_capture_free(&cap);
}

void test_bb_system_reboot_route_empty_body_returns_200(void)
{
    bb_http_host_capture_t cap = run_reboot("", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Valid body — {"ts": ..., "detail": ...} plumbed through to the parse fn;
// still just a 200 {"status":"rebooting"} response either way (the parsed
// ts/detail feed the device-only restart call, not the HTTP response body).
// ---------------------------------------------------------------------------

void test_bb_system_reboot_route_valid_ts_returns_200(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":1700000000}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_bb_system_reboot_route_detail_present_returns_200(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"detail\":\"user requested\"}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);
}

// detail absent, User-Agent header present -- the route resolves the header
// once up front and passes it as the parse fn's ua_or_null fallback. Assert
// the RESOLVED detail via the BB_SYSTEM_TESTING capture seam (the response
// body never carries it -- see bb_system_reboot_capture_get_for_test()'s
// doc), so this can actually fail if the fallback plumbing regresses.
void test_bb_system_reboot_route_detail_absent_falls_back_to_user_agent(void)
{
    bb_http_host_capture_t cap = run_reboot("{}", "curl/8.0");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    TEST_ASSERT_TRUE(bb_system_reboot_capture_get_for_test(resolved_detail, sizeof(resolved_detail), &resolved_ts));
    TEST_ASSERT_EQUAL_STRING("curl/8.0", resolved_detail);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// No body at all, User-Agent header present -- same fallback path, driven
// from the no-body branch instead of an empty JSON object.
void test_bb_system_reboot_route_no_body_falls_back_to_user_agent(void)
{
    bb_http_host_capture_t cap = run_reboot(NULL, "curl/8.0");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    TEST_ASSERT_TRUE(bb_system_reboot_capture_get_for_test(resolved_detail, sizeof(resolved_detail), &resolved_ts));
    TEST_ASSERT_EQUAL_STRING("curl/8.0", resolved_detail);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// detail present in body wins over the User-Agent header (the header is
// only consulted when detail is absent).
void test_bb_system_reboot_route_detail_present_wins_over_user_agent(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"detail\":\"user requested\"}", "curl/8.0");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    TEST_ASSERT_TRUE(bb_system_reboot_capture_get_for_test(resolved_detail, sizeof(resolved_detail), &resolved_ts));
    TEST_ASSERT_EQUAL_STRING("user requested", resolved_detail);
}

// No detail, no User-Agent header -- resolved detail is empty (nothing to
// fall back to), not a crash / stale value.
void test_bb_system_reboot_route_no_detail_no_user_agent_resolves_empty(void)
{
    bb_http_host_capture_t cap = run_reboot("{}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    TEST_ASSERT_TRUE(bb_system_reboot_capture_get_for_test(resolved_detail, sizeof(resolved_detail), &resolved_ts));
    TEST_ASSERT_EQUAL_STRING("", resolved_detail);
}

// ts arrives resolved as-parsed (no User-Agent fallback for ts -- only
// detail has one).
void test_bb_system_reboot_route_valid_ts_resolves_via_capture(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":1700000000}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    TEST_ASSERT_TRUE(bb_system_reboot_capture_get_for_test(resolved_detail, sizeof(resolved_detail), &resolved_ts));
    TEST_ASSERT_EQUAL_UINT32(1700000000, resolved_ts);
}

// ---------------------------------------------------------------------------
// Malformed body — tolerated today (200), NOT rejected. PR1 must not change
// this: it only touches build wiring + the restart-call gate, never the
// route's own error handling.
// ---------------------------------------------------------------------------

void test_bb_system_reboot_route_malformed_body_returns_200(void)
{
    bb_http_host_capture_t cap = run_reboot("not-json", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);
}

// Oversized body: the handler's own BB_SYSTEM_REBOOT_BODY_MAX bound (256
// bytes) is exceeded, so raw_len > BB_SYSTEM_REBOOT_BODY_MAX and the body is
// silently skipped (treated the same as no body) -- still tolerated, 200.
void test_bb_system_reboot_route_oversized_body_returns_200(void)
{
    char body[300];
    memset(body, 'x', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';

    bb_http_host_capture_t cap = run_reboot(body, NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Error branches newly exposed by host-compiling this file (B1-1148 review
// finding 2). recv failure and the JSON-object-stream begin/end failures use
// the existing bb_http_host_force_*() hooks (platform/host/bb_http_server).
// ---------------------------------------------------------------------------

// raw_len > 0 but bb_http_req_recv() itself fails (n <= 0) -- the handler
// must fall back to "no body" rather than reading garbage/uninitialized
// data, same as the true-no-body case.
void test_bb_system_reboot_route_recv_fail_treated_as_no_body(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body("{\"detail\":\"user requested\"}", strlen("{\"detail\":\"user requested\"}"));
    bb_http_host_set_req_header("User-Agent", NULL);

    bb_http_host_force_recv_fail(true);
    bb_err_t rc = bb_system_reboot_handler_for_test(req);
    bb_http_host_force_recv_fail(false);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    TEST_ASSERT_TRUE(bb_system_reboot_capture_get_for_test(resolved_detail, sizeof(resolved_detail), &resolved_ts));
    TEST_ASSERT_EQUAL_STRING("", resolved_detail);
}

// bb_http_resp_json_obj_begin() failure (via its bb_http_resp_set_type()
// call) propagates straight out of reboot_handler -- no response body is
// ever built, and the capture point is never reached (it sits after
// obj_end()).
void test_bb_system_reboot_route_json_obj_begin_fail_propagates(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_set_req_header("User-Agent", NULL);

    bb_http_host_force_set_type_fail(true);
    bb_err_t rc = bb_system_reboot_handler_for_test(req);
    bb_http_host_force_set_type_fail(false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_FALSE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}

// bb_http_resp_json_obj_end() failure (the terminator send_chunk(NULL,0)
// call) propagates as reboot_handler's own return value -- the capture
// point still runs (it sits right after obj_end(), unconditionally), same
// as the success path.
void test_bb_system_reboot_route_json_obj_end_fail_propagates(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_set_req_header("User-Agent", NULL);

    bb_http_host_force_send_chunk_term_fail(true);
    bb_err_t rc = bb_system_reboot_handler_for_test(req);
    bb_http_host_force_send_chunk_term_fail(false);
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_TRUE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}
