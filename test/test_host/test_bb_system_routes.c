// Tests for the POST /api/reboot route handler (B1-1148 PR2).
//
// PR2 migrates the request body off the hand-rolled
// bb_system_reboot_parse_body() (deleted) onto a "reboot" bb_data ingress
// binding driven through bb_data_apply() -- same template as
// test_bb_storage_http_factory_reset.c's factory_reset_handler coverage.
// This file drives the REAL production binding via
// bb_system_reboot_bind_for_test() (bb_system_routes.c), not a host-only
// mirror -- bb_system_routes.c is already fully host-compiled (PR1).
//
// The device-only restart call (bb_system_restart_reason_at(), whose host
// stub calls exit(0)) is compiled out on host via #ifdef ESP_PLATFORM in
// bb_system_routes.c -- these tests never reach it, by construction.
//
// Because that call is the only consumer of the resolved (detail, ts) pair,
// and the HTTP response body never carries them, the User-Agent-fallback /
// ts-clamp / "what actually got resolved" tests below assert via a
// dedicated BB_SYSTEM_TESTING capture seam (bb_system_reboot_capture_get_
// for_test()) that records the resolved values immediately before the
// restart call -- same shape as bb_storage_http's s_erase_all_calls
// fixture. This makes them load-bearing: a fallback/plumbing/clamp
// regression turns them red, not just a status-code check that any of
// these tests would already pass. Reset in setUp() via
// bb_system_reboot_capture_reset_for_test() (test_main.c).
//
// BREAKING CHANGE (B1-1148, user-approved): a body that was actually SENT
// but fails to parse now returns 400 (previously tolerated, 200, ts=0,
// detail falls back to the User-Agent header). An absent or empty body is
// UNCHANGED -- still tolerated, 200 -- see the malformed-body section below.
//
// ts clamp (B1-1148 finding 3): the deleted bb_system_reboot_parse_body()
// clamped a DOUBLE ("ts") to (0, UINT32_MAX] before casting. The new "reboot"
// binding declares "ts" as BB_TYPE_U64, whose populate path only ever sees
// an int64_t/uint64_t (bb_serialize_json_tok_get_i64(), itself backed by
// strtoll() on the number's raw text -- see
// bb_serialize_json_tok.c:tok_parse_num()), never the double. For every
// PLAIN-INTEGER edge case (negative, zero, a huge plain integer, a value
// just over UINT32_MAX) this is observably IDENTICAL to the old double-based
// clamp -- see the tests below, plus the route's own doc comment for why a
// negative value needs no separate sign check.
//
// ts divergence guard (B1-1148 finding 1, user-approved, PR2 review): the
// scientific-notation divergence above is broader than plain "astronomically
// large" values -- strtoll() stops at the first non-digit character
// regardless of magnitude, so "ts":"5e1" (== 50, an ORDINARY in-range value)
// silently parses as 5, and "ts":"1.5e10" (== 15000000000, correctly
// out-of-range) silently parses as 1 and is wrongly ACCEPTED as in-range.
// The user's decision: any "ts" that isn't a clean base-10 integer the
// parse can honour exactly now returns 400 instead of silently mis-clamping
// -- see reboot_apply()'s doc comment (bb_system_routes.c) for the detection
// mechanism (a shadow BB_TYPE_F64 read of the same "ts" key, compared
// against the BB_TYPE_U64 read). See
// test_bb_system_reboot_route_ts_scientific_notation_returns_400,
// test_bb_system_reboot_route_ts_5e1_returns_400, and
// test_bb_system_reboot_route_ts_decimal_exponent_returns_400 below.

#include "unity.h"
#include "bb_system.h"
#include "bb_system_test.h"
#include "bb_data.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helper: bind the "reboot" bb_data key against the production gather/apply
// hooks (bb_system_reboot_bind_for_test(), platform/espidf/bb_system/
// bb_system_routes.c). bb_data_test_reset() wipes the WHOLE binding table
// (shared process-lifetime state, same posture as every other bb_data-fed
// route's own test file) -- called before every test below, even ones that
// never actually reach bb_data_apply() (an absent/empty body short-circuits
// before that call), so this file never depends on run order.
// ---------------------------------------------------------------------------

static void bind_bb_data(void)
{
    bb_data_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_system_reboot_bind_for_test());
}

// ---------------------------------------------------------------------------
// Helper: run the reboot handler with an optional JSON body / User-Agent
// header, returning the captured response.
// ---------------------------------------------------------------------------

static bb_http_host_capture_t run_reboot(const char *body, const char *user_agent)
{
    bind_bb_data();

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

// Resolves the (detail, ts) capture into out params, asserting it was
// actually recorded (i.e. reboot_handler() reached the capture point).
static void get_resolved(char *out_detail, size_t out_detail_size, uint32_t *out_ts)
{
    TEST_ASSERT_TRUE(bb_system_reboot_capture_get_for_test(out_detail, out_detail_size, out_ts));
}

// ---------------------------------------------------------------------------
// Absent / empty body — no ts, no detail, still 200. Never reaches
// bb_data_apply() at all (body_len stays 0) -- UNCHANGED by PR2.
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

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_STRING("", resolved_detail);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

void test_bb_system_reboot_route_empty_body_returns_200(void)
{
    bb_http_host_capture_t cap = run_reboot("", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);
}

// A valid-but-empty JSON object ("{}") DOES reach bb_data_apply() (body_len
// > 0) -- distinct code path from the true-no-body case above, but the same
// observable outcome (both fields absent from the object).
void test_bb_system_reboot_route_empty_object_body_returns_200(void)
{
    bb_http_host_capture_t cap = run_reboot("{}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_STRING("", resolved_detail);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// An unknown field is silently ignored -- bb_serialize_populate() only ever
// looks up the field keys the descriptor declares ("ts"/"detail"); a field
// present in the body under any other key is simply never queried.
void test_bb_system_reboot_route_unknown_field_ignored(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"foo\":\"bar\"}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_STRING("", resolved_detail);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// ---------------------------------------------------------------------------
// Valid body — {"ts": ..., "detail": ...} plumbed through bb_data_apply();
// still just a 200 {"status":"rebooting"} response either way (the resolved
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
// once up front and merges it into dst_scratch.detail AFTER bb_data_apply()
// returns. Assert the RESOLVED detail via the BB_SYSTEM_TESTING capture seam
// (the response body never carries it), so this can actually fail if the
// fallback plumbing regresses.
void test_bb_system_reboot_route_detail_absent_falls_back_to_user_agent(void)
{
    bb_http_host_capture_t cap = run_reboot("{}", "curl/8.0");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
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
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_STRING("curl/8.0", resolved_detail);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// detail present in body wins over the User-Agent header (the header is
// only consulted when the resolved detail is empty).
void test_bb_system_reboot_route_detail_present_wins_over_user_agent(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"detail\":\"user requested\"}", "curl/8.0");
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 0;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
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
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
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
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_UINT32(1700000000, resolved_ts);
}

// ---------------------------------------------------------------------------
// ts clamp edge cases (B1-1148 finding 3) -- valid range is (0, UINT32_MAX],
// same as the deleted bb_system_reboot_parse_body()'s double-based clamp.
// Each of these was empirically verified against a standalone strtoll()
// probe (see this file's header comment) before being written, per the PR's
// "do not assume" instruction.
// ---------------------------------------------------------------------------

// A negative value arrives via BB_TYPE_U64 as its int64_t bit pattern
// reinterpreted as uint64_t (populate_get_u64() casts get_i64()'s result) --
// e.g. -5 becomes 0xFFFFFFFFFFFFFFFB, far above UINT32_MAX, so the route's
// single upper-bound check excludes it with no separate sign check needed.
void test_bb_system_reboot_route_ts_negative_clamps_to_zero(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":-5}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 999;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

void test_bb_system_reboot_route_ts_zero_stays_zero(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":0}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 999;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// A huge PLAIN-INTEGER value (no exponent, 20 digits) overflows int64_t;
// strtoll() saturates to INT64_MAX on overflow (empirically confirmed:
// `strtoll("99999999999999999999", NULL, 10) == INT64_MAX`) -- cast to
// uint64_t is the same huge value, still far above UINT32_MAX, so the route
// clamps it to 0, byte-identical to the deleted double-based clamp's
// ts_num > (double)UINT32_MAX rejection of the same input.
void test_bb_system_reboot_route_ts_huge_plain_integer_clamps_to_zero(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":99999999999999999999}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 999;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// ts divergence guard (B1-1148 finding 1, user-approved, PR2 review): "ts"
// in scientific notation now returns 400 rather than silently mis-parsing.
// The old double-based clamp parsed "1e300" as an astronomically large
// double and correctly clamped it to 0; the naive BB_TYPE_U64-only path
// would have parsed it as the plain integer 1 (strtoll() stops at the first
// non-digit character -- empirically confirmed: `strtoll("1e300", NULL, 10)
// == 1`) and wrongly resolved a VALID, in-range ts. reboot_apply()'s ts_f64
// shadow field (bb_system_routes.c) catches the mismatch (1.0 != 1e300) and
// rejects before it ever reaches the clamp -- see reboot_apply()'s doc
// comment for the detection mechanism.
void test_bb_system_reboot_route_ts_scientific_notation_returns_400(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":1e300}", NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    // Rejected before the capture point -- reboot_apply() fails inside
    // bb_data_apply(), before reboot_handler() ever reaches the User-Agent
    // fallback / ts clamp / capture.
    TEST_ASSERT_FALSE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}

// The silently-CORRUPTED case (as opposed to the silently out-of-range case
// above): "5e1" == 50, an ORDINARY, in-range value if parsed correctly.
// strtoll() stops at 'e' and reads it as the plain integer 5 instead --
// wrong, not just wrongly-ranged. Proves the fix actually catches an
// in-range false parse, not just an out-of-range one.
void test_bb_system_reboot_route_ts_5e1_returns_400(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":5e1}", NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_FALSE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}

// Fraction + exponent together: "1.5e10" == 15000000000, correctly
// out-of-range under the old double-based clamp (clamped to 0). strtoll()
// stops at '.' and reads it as the plain integer 1 -- silently ACCEPTED as
// in-range instead of rejected. Proves the fix catches the "silently
// accepted when it should have been out-of-range" case too.
void test_bb_system_reboot_route_ts_decimal_exponent_returns_400(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":1.5e10}", NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_FALSE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}

// The HEADLINE case (B1-1148 finding 2, PR2 review): a bare plain FRACTION,
// no exponent at all. "1.9" == 1.9, strtoll() stops at '.' and reads it as
// the plain integer 1 -- an ordinary in-range mismatch, same shape as "5e1"
// above but exercising the "or exponent" -> "a fraction" half of
// reboot_apply()'s own doc comment, which no test previously covered (every
// prior divergence test used an exponent).
void test_bb_system_reboot_route_ts_bare_fraction_returns_400(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":1.9}", NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_FALSE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}

// B1-1148 finding 1's concrete escape input: "ts"'s digit PREFIX (before
// the exponent) is exactly INT64_MAX's decimal text, so strtoll() -- which
// stops at 'e' -- returns INT64_MAX WITHOUT any actual overflow, landing on
// the exact same sentinel value a genuine saturating overflow (see the
// "huge plain integer" test above) would produce. reboot_apply() cannot
// tell the two apart from (i64, f64) alone (see its doc comment) and
// instead resolves by RANGE: the true full-grammar value here
// (9223372036854775807e1 == ~9.223e19) is itself astronomically outside
// the accepted (0, UINT32_MAX] range, so it clamps to 0 -- 200, matching
// what the OLD (pre-migration) double-based clamp would have done for this
// exact input, and matching this route's posture for every other
// out-of-range huge value (see the "huge plain integer" and
// "over_uint32_max" tests). This is deliberately NOT a 400: see
// test_bb_system_reboot_route_ts_sentinel_prefix_shrunk_into_range_returns_400
// below for the genuinely dangerous sibling case this same fix rejects.
void test_bb_system_reboot_route_ts_sentinel_prefix_with_exponent_clamps_to_zero(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":9223372036854775807e1}", NULL);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 999;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// The genuinely dangerous sibling of the test above: an exponent that
// drags a would-be-INT64_MAX-sentinel digit prefix back DOWN into a
// plausible, in-range value (9223372036854775807e-18 == ~9.223, an
// ordinary, meaningful timestamp) instead of up out of range. Proves
// reboot_apply()'s range-based resolution actually closes B1-1148 finding
// 1's escape rather than merely relocating it: since the true value here
// IS in (0, UINT32_MAX], the sentinel can only have been reached by
// truncating this exponent, so it is rejected -- silently discarding this
// input to ts=0 (what the pre-fix code did, since ts_i64 == INT64_MAX
// exempted it unconditionally) would have thrown away a real timestamp.
void test_bb_system_reboot_route_ts_sentinel_prefix_shrunk_into_range_returns_400(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":9223372036854775807e-18}", NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_FALSE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}

// A value one above UINT32_MAX -- the upper-bound half of the (0, UINT32_MAX]
// range, distinct from the "huge" overflow case above (this one fits
// comfortably in an int64_t/uint64_t, no strtoll() saturation involved).
void test_bb_system_reboot_route_ts_over_uint32_max_clamps_to_zero(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"ts\":4294967296}", NULL);  // UINT32_MAX + 1
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);

    char resolved_detail[49];
    uint32_t resolved_ts = 999;
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_UINT32(0, resolved_ts);
}

// ---------------------------------------------------------------------------
// Malformed body — BREAKING CHANGE (B1-1148, user-approved): a body that was
// actually SENT but fails to parse now returns 400, rejected BEFORE the
// User-Agent fallback / ts clamp / capture point ever run. Previously
// tolerated (200) -- see this route's own doc comment.
// ---------------------------------------------------------------------------

void test_bb_system_reboot_route_malformed_body_returns_400(void)
{
    bb_http_host_capture_t cap = run_reboot("not-json", NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);

    cJSON *j = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(j, "400 body is not valid JSON");
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(cJSON_IsString(err));
    cJSON_Delete(j);

    bb_http_host_capture_free(&cap);

    // Rejected before the capture point -- the handler never reaches it.
    TEST_ASSERT_FALSE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}

// A truncated body (no closing brace) fails the JSON scan mid-parse rather
// than at the grammar-check-the-whole-thing-up-front stage the "not-json"
// case above exercises -- BB_ERR_PARSE_INCOMPLETE, the disjoint parse-layer
// code from BB_ERR_PARSE_GRAMMAR (bb_core.h, B1-1090). The route maps both
// to 400 -- this pins that a truncated body doesn't fall through to the
// generic 500 branch instead.
void test_bb_system_reboot_route_truncated_body_returns_400(void)
{
    bb_http_host_capture_t cap = run_reboot("{\"detail\":\"x\"", NULL);
    TEST_ASSERT_EQUAL_INT(400, cap.status);
    bb_http_host_capture_free(&cap);

    TEST_ASSERT_FALSE(bb_system_reboot_capture_get_for_test(NULL, 0, NULL));
}

// Oversized body: the handler's own BB_SYSTEM_REBOOT_BODY_MAX bound (256
// bytes) is exceeded, so raw_len > BB_SYSTEM_REBOOT_BODY_MAX and the body is
// silently skipped (treated the same as no body) -- never reaches
// bb_data_apply() at all, still tolerated, 200. UNCHANGED by PR2 (the new
// 400 is only for a body that was actually READ and then failed to parse).
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
// Error branches newly exposed by host-compiling this file (B1-1148 PR1
// review finding 2). recv failure and the JSON-object-stream begin/end
// failures use the existing bb_http_host_force_*() hooks (platform/host/
// bb_http_server). None of these bodies are long enough to reach
// bb_data_apply() (recv failure yields body_len == 0; the other two pass no
// body at all), so they don't depend on the "reboot" binding being bound --
// bind_bb_data() is still called (via run_reboot() where used) for
// consistency with every other test in this file.
// ---------------------------------------------------------------------------

// raw_len > 0 but bb_http_req_recv() itself fails (n <= 0) -- the handler
// must fall back to "no body" rather than reading garbage/uninitialized
// data, same as the true-no-body case.
void test_bb_system_reboot_route_recv_fail_treated_as_no_body(void)
{
    bind_bb_data();

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
    get_resolved(resolved_detail, sizeof(resolved_detail), &resolved_ts);
    TEST_ASSERT_EQUAL_STRING("", resolved_detail);
}

// bb_http_resp_json_obj_begin() failure (via its bb_http_resp_set_type()
// call) propagates straight out of reboot_handler -- no response body is
// ever built, and the capture point is never reached (it sits after
// obj_end()).
void test_bb_system_reboot_route_json_obj_begin_fail_propagates(void)
{
    bind_bb_data();

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
    bind_bb_data();

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

// ---------------------------------------------------------------------------
// reboot_gather() coverage (B1-1148 PR2 review finding 2) -- mirrors
// test_bb_storage_http_factory_reset.c's
// test_storage_http_factory_reset_gather_stub_reachable_via_patch_mode.
// Production always applies via BB_DATA_APPLY_POST (the route hardcodes
// this), so reboot_gather() is never invoked by the HTTP handler -- same
// "exists only to satisfy bb_data_bind()'s non-NULL-gather invariant"
// posture as factory_reset_gather()/wifi_creds_gather(). This drives the
// SAME bound "reboot" key through bb_data_apply() directly in
// BB_DATA_APPLY_PATCH mode (bypassing the HTTP handler, which never uses
// this mode) purely to reach the gather stub's memset0 body --
// platform/espidf/ is outside the coverage gate's FILTERS, so nothing else
// will ever catch a regression in this line.
// ---------------------------------------------------------------------------

void test_bb_system_reboot_route_gather_stub_reachable_via_patch_mode(void)
{
    bind_bb_data();

    char body[]           = "{\"ts\":1700000000}";
    char dst_scratch[128];
    char parse_scratch[3072];
    bb_data_apply_req_t req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "reboot",
        .mode              = BB_DATA_APPLY_PATCH,
        .body              = body,
        .body_len          = strlen(body),
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };

    TEST_ASSERT_EQUAL(BB_OK, bb_data_apply(&req));
}
