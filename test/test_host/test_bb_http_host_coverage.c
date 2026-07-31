#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "bb_mem_test.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// B1-1279 PR3 residual coverage. Editing platform/host/bb_http_server/
// bb_http_host.c shifted every downstream line number, which turns this
// file's whole set of pre-existing (already-baselined, unrelated) uncovered
// branches into "new" markers under the per-file coverage ratchet (see
// scripts/coverage_baseline.py's module docstring: "touch it, bring it to
// 100, by construction, not by policy"). These tests close both those
// pre-existing gaps and the genuinely-new branches introduced by this PR's
// two new host mirrors (bb_http_host_api_dispatch, bb_http_host_dispatch_route)
// and the non-/api route registry backing bb_http_host_dispatch_route.
// ---------------------------------------------------------------------------

static bb_err_t ok_handler(bb_http_request_t *req)
{
    bb_http_resp_set_status(req, 200);
    return BB_OK;
}

static void teardown(void)
{
    bb_http_host_reset_routes();
    bb_http_host_reset_assets();
    bb_dispatch_api_reset();
    bb_mem_set_alloc_hook(NULL);
    bb_http_host_force_set_type_fail(false);
    bb_http_host_force_send_chunk_fail(false);
}

// ===========================================================================
// bb_http_register_route (non-/api registry) + bb_http_host_dispatch_route
// ===========================================================================

void test_bb_http_host_register_route_non_api_overflow_silently_dropped(void)
{
    static char paths[32][24];
    // BB_HTTP_HOST_ROUTE_CAP is 16 -- fill it, then one more.
    for (int i = 0; i < 16; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/fill%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route(NULL, BB_HTTP_GET, paths[i], ok_handler));
    }
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route(NULL, BB_HTTP_GET, "/overflow", ok_handler));

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/overflow");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);  // dropped -- never reached the registry

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_host_register_route_null_path_and_handler_tolerated(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route(NULL, BB_HTTP_GET, NULL, ok_handler));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route(NULL, BB_HTTP_GET, "/no-handler", NULL));
    teardown();
}

void test_bb_http_host_dispatch_route_null_req_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_dispatch_route(NULL, BB_HTTP_GET, "/x"));
    teardown();
}

void test_bb_http_host_dispatch_route_null_uri_invalid_arg(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_dispatch_route(req, BB_HTTP_GET, NULL));
    teardown();
}

void test_bb_http_host_dispatch_route_method_mismatch_denies_404(void)
{
    bb_http_register_route(NULL, BB_HTTP_GET, "/x", ok_handler);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_POST, "/x");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_host_dispatch_route_path_mismatch_denies_404(void)
{
    bb_http_register_route(NULL, BB_HTTP_GET, "/x", ok_handler);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/y");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_host_dispatch_route_never_registered_denies_404(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/nope");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

// ===========================================================================
// bb_http_host_api_dispatch
// ===========================================================================

void test_bb_http_host_api_dispatch_null_req_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_api_dispatch(NULL, BB_HTTP_GET, "/api/x"));
    teardown();
}

void test_bb_http_host_api_dispatch_null_uri_invalid_arg(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_api_dispatch(req, BB_HTTP_GET, NULL));
    teardown();
}

void test_bb_http_host_api_dispatch_hit_with_null_handler_returns_501(void)
{
    // bb_dispatch_api_add has no NULL-handler guard (defensive-only branch,
    // same as the device's api_dispatch_handler comment) -- register directly
    // through the plain host path to exercise it.
    bb_http_register_route(NULL, BB_HTTP_GET, "/api/nullhandler", NULL);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_GET, "/api/nullhandler");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(501, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_host_api_dispatch_method_mismatch_returns_405(void)
{
    bb_http_register_route(NULL, BB_HTTP_GET, "/api/x", ok_handler);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_POST, "/api/x");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(405, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_host_api_dispatch_miss_returns_404(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_GET, "/api/nope");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

// ===========================================================================
// bb_http_resp_set_type / bb_http_resp_set_header — missing branch: the
// second half of the "cap && x" guard.
// ===========================================================================

void test_bb_http_resp_set_type_null_mime_leaves_content_type_unset(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_resp_set_type(req, NULL);
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("", cap.content_type);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_resp_set_header_null_key_noop(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_resp_set_header(req, NULL, "value");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(cap.has_acao);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_resp_set_header_no_active_capture_noop(void)
{
    int dummy;
    bb_err_t err = bb_http_resp_set_header((bb_http_request_t *)&dummy, "Content-Type", "value");
    TEST_ASSERT_EQUAL(BB_OK, err);
    teardown();
}

// ===========================================================================
// bb_http_req_body_len / bb_http_req_recv — missing branch: capture active
// but no body injected.
// ===========================================================================

void test_bb_http_req_body_len_no_body_injected_returns_zero(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    TEST_ASSERT_EQUAL(0, bb_http_req_body_len(req));
    teardown();
}

void test_bb_http_req_body_len_no_active_capture_returns_zero(void)
{
    int dummy;
    TEST_ASSERT_EQUAL(0, bb_http_req_body_len((bb_http_request_t *)&dummy));
    teardown();
}

void test_bb_http_req_recv_truncates_to_buf_size(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body("0123456789", 10);

    char buf[4];
    int n = bb_http_req_recv(req, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_MEMORY("0123", buf, 4);

    teardown();
}

void test_bb_http_req_recv_body_injected_but_zero_length_returns_zero(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    // Non-NULL body pointer but zero length -- exercises the
    // "req_body non-NULL, req_body_len > 0" condition's false arm.
    bb_http_host_capture_set_req_body("ignored", 0);

    char buf[8];
    TEST_ASSERT_EQUAL(0, bb_http_req_recv(req, buf, sizeof(buf)));
    teardown();
}

// ===========================================================================
// bb_http_host_serve_asset
// ===========================================================================

void test_bb_http_host_serve_asset_null_args_invalid_arg(void)
{
    static const uint8_t data[] = { 'x' };
    static const bb_http_asset_t asset = {
        .path = "/x", .mime = "text/plain", .encoding = NULL, .data = data, .len = 1,
    };
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_serve_asset(NULL, &asset));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_serve_asset(req, NULL));

    teardown();
}

void test_bb_http_host_serve_asset_propagates_set_type_failure(void)
{
    static const uint8_t data[] = { 'x' };
    static const bb_http_asset_t asset = {
        .path = "/x", .mime = "text/plain", .encoding = NULL, .data = data, .len = 1,
    };
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_force_set_type_fail(true);

    bb_err_t err = bb_http_host_serve_asset(req, &asset);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    teardown();
}

// ===========================================================================
// bb_http_host_asset_wildcard — pre-existing null-arg + query-truncation
// branches (shifted, not new to this PR).
// ===========================================================================

void test_bb_http_host_asset_wildcard_null_args_invalid_arg(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_asset_wildcard(NULL, "/x"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_asset_wildcard(req, NULL));
    teardown();
}

void test_bb_http_host_asset_wildcard_long_path_query_truncated_to_404(void)
{
    // Path portion (before '?') exceeds the 256-byte path_buf -- exercises
    // the truncation guard's TRUE arm. No asset can match a path this long,
    // so this always 404s; the point is exercising the truncation branch
    // itself without an out-of-bounds copy.
    static char long_uri[512];
    memset(long_uri, 'a', sizeof(long_uri) - 8);
    long_uri[sizeof(long_uri) - 8] = '?';
    memcpy(long_uri + sizeof(long_uri) - 7, "q=1", 4);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, long_uri);
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

// ===========================================================================
// bb_http_resp_sendstr / bb_http_resp_send_chunk
// ===========================================================================

void test_bb_http_resp_sendstr_null_str_sends_empty(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_resp_sendstr(req, NULL);
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NULL(cap.body);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_resp_sendstr_propagates_send_chunk_failure(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_force_send_chunk_fail(true);

    bb_err_t err = bb_http_resp_sendstr(req, "x");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    teardown();
}

void test_bb_http_resp_send_chunk_zero_length_buf_skips_growth(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_resp_send_chunk(req, "", 0);
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NULL(cap.body);

    bb_http_host_capture_free(&cap);
    teardown();
}

static void *failing_realloc_hook(size_t sz)
{
    (void)sz;
    return NULL;
}

void test_bb_http_resp_send_chunk_realloc_failure_leaves_body_unchanged(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_mem_set_alloc_hook(failing_realloc_hook);

    bb_err_t err = bb_http_resp_send_chunk(req, "data", -1);
    bb_mem_set_alloc_hook(NULL);
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);   // failure is swallowed, not propagated
    TEST_ASSERT_NULL(cap.body);

    bb_http_host_capture_free(&cap);
    teardown();
}

// ===========================================================================
// bb_http_resp_no_content / bb_http_req_uri — missing branch: no active
// capture for the given req.
// ===========================================================================

void test_bb_http_resp_no_content_no_active_capture_noop(void)
{
    int dummy;
    bb_err_t err = bb_http_resp_no_content((bb_http_request_t *)&dummy);
    TEST_ASSERT_EQUAL(BB_OK, err);
    teardown();
}

void test_bb_http_req_uri_no_active_capture_defaults_empty(void)
{
    int dummy;
    char out[16];
    bb_err_t err = bb_http_req_uri((bb_http_request_t *)&dummy, out, sizeof(out));
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("", out);
    teardown();
}

// ===========================================================================
// bb_http_req_query_key_value / bb_http_req_query_has_key
// ===========================================================================

void test_bb_http_req_query_key_value_bare_key_zero_outlen_invalid_arg(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("schema");

    bb_err_t err = bb_http_req_query_key_value(req, "schema", NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);

    teardown();
}

void test_bb_http_req_query_key_value_segment_shorter_than_key_skipped(void)
{
    // "ab" is shorter than the 6-char key "schema" -- exercises the
    // seg_len >= klen guard's FALSE arm (falls through to no-match).
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_string("ab");

    char out[16];
    bb_err_t err = bb_http_req_query_key_value(req, "schema", out, sizeof(out));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);

    teardown();
}

void test_bb_http_req_query_key_value_legacy_mismatch_invalid_arg(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_param("foo", "bar");

    char out[16];
    bb_err_t err = bb_http_req_query_key_value(req, "other", out, sizeof(out));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);

    teardown();
}

void test_bb_http_req_query_key_value_legacy_key_matches_but_val_null_invalid_arg(void)
{
    // query_key matches but query_val was never set (NULL) -- exercises the
    // "cap->query_val" guard's FALSE arm.
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_param("foo", NULL);

    char out[16];
    bb_err_t err = bb_http_req_query_key_value(req, "foo", out, sizeof(out));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);

    teardown();
}

void test_bb_http_req_query_has_key_legacy_mismatch_false(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_query_param("foo", "bar");

    TEST_ASSERT_FALSE(bb_http_req_query_has_key(req, "other"));

    teardown();
}

void test_bb_http_req_query_has_key_no_query_key_set_false(void)
{
    // Fresh capture, no query param ever injected -- exercises
    // cap->query_key's FALSE arm.
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);

    TEST_ASSERT_FALSE(bb_http_req_query_has_key(req, "anything"));

    teardown();
}

// ===========================================================================
// Capture harness edge branches
// ===========================================================================

void test_bb_http_host_capture_begin_null_out_req_noop(void)
{
    bb_http_host_capture_begin(NULL);  // must not crash
    teardown();
}

void test_bb_http_host_capture_set_req_body_null_body_zeroes_len(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(NULL, 5);
    TEST_ASSERT_EQUAL(0, bb_http_req_body_len(req));
    teardown();
}

void test_bb_http_host_capture_end_mismatched_req_invalid_arg(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);

    int other;
    bb_http_host_capture_t cap;
    bb_err_t err = bb_http_host_capture_end((bb_http_request_t *)&other, &cap);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);

    // Slot is still armed for the real req -- disarm it properly.
    bb_http_host_capture_end(req, &cap);
    bb_http_host_capture_free(&cap);
    teardown();
}

void test_bb_http_host_capture_free_null_cap_noop(void)
{
    bb_http_host_capture_free(NULL);  // must not crash
    teardown();
}
