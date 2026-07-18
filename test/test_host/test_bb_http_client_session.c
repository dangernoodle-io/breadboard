// Tests for bb_http_client session API (keep-alive session):
// - session_open NULL-arg guards
// - session_post NULL-arg guards
// - session_close NULL safe
// - session_post records url/body/content_type
// - session_post default content-type application/json
// - session_post transport error passthrough
// - session_post 2xx returns BB_OK; non-2xx returns BB_ERR_INVALID_STATE
// - clear_mock resets session record
#include "unity.h"
#include "bb_http_client.h"
#include "bb_serialize.h"
#include "../../platform/host/bb_http_client/bb_http_client_host.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static void reset(void)
{
    bb_http_client_clear_mock();
}

// -------------------------------------------------------------------------
// session_open NULL-arg guards
// -------------------------------------------------------------------------

void test_bb_http_client_session_open_null_url_base_returns_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_open(NULL, NULL, &s));
}

void test_bb_http_client_session_open_null_out_returns_invalid_arg(void)
{
    reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_open(NULL, "http://x", NULL));
}

void test_bb_http_client_session_open_succeeds(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));
    TEST_ASSERT_NOT_NULL(s);
    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// session_post NULL-arg guards
// -------------------------------------------------------------------------

void test_bb_http_client_session_post_null_session_returns_invalid_arg(void)
{
    reset();
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_post(NULL, "http://x", "{}", 2, NULL, &r));
}

void test_bb_http_client_session_post_null_url_returns_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));
    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_post(s, NULL, "{}", 2, NULL, &r));
    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_null_out_returns_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_post(s, "http://x", "{}", 2, NULL, NULL));
    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// session_close NULL safe
// -------------------------------------------------------------------------

void test_bb_http_client_session_close_null_is_safe(void)
{
    bb_http_client_session_close(NULL);  // must not crash
}

// -------------------------------------------------------------------------
// session_post records captured values
// -------------------------------------------------------------------------

void test_bb_http_client_session_post_captures_url_body_content_type(void)
{
    reset();
    bb_http_client_session_set_mock_status(200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    const char *body = "{\"ok\":true}";
    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub",
                                    body, strlen(body),
                                    "application/json", &r));
    TEST_ASSERT_EQUAL(200, r.status_code);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);
    TEST_ASSERT_EQUAL_STRING("http://example.com/pub", rec.url);
    TEST_ASSERT_EQUAL_PTR(body, rec.body);
    TEST_ASSERT_EQUAL(strlen(body), rec.body_len);
    TEST_ASSERT_EQUAL_STRING("application/json", rec.content_type);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_default_content_type_application_json(void)
{
    reset();
    bb_http_client_session_set_mock_status(200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub",
                                    NULL, 0, NULL, &r));

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_EQUAL_STRING("application/json", rec.content_type);

    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// Transport error passthrough
// -------------------------------------------------------------------------

void test_bb_http_client_session_post_transport_error_passthrough(void)
{
    reset();
    bb_http_client_session_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
        bb_http_client_session_post(s, "http://example.com/pub",
                                    "{}", 2, NULL, &r));
    TEST_ASSERT_EQUAL(0, r.status_code);

    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// 2xx → BB_OK; non-2xx → BB_ERR_INVALID_STATE
// -------------------------------------------------------------------------

void test_bb_http_client_session_post_2xx_returns_ok(void)
{
    reset();
    bb_http_client_session_set_mock_status(201);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r = {0};
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub",
                                    "{}", 2, NULL, &r));
    TEST_ASSERT_EQUAL(201, r.status_code);

    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// clear_mock resets session record
// -------------------------------------------------------------------------

void test_bb_http_client_session_clear_mock_resets_record(void)
{
    reset();
    bb_http_client_session_set_mock_status(200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r = {0};
    bb_http_client_session_post(s, "http://example.com/pub", "{}", 2, NULL, &r);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);

    bb_http_client_clear_mock();
    rec = bb_http_client_session_last_post();
    TEST_ASSERT_FALSE(rec.called);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_tls_error_code_propagated(void)
{
    bb_http_client_session_t sess;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "https://example.com", &sess));
    bb_http_client_session_set_mock_tls_error_code(-0x7200);
    bb_http_client_session_set_mock_status(200);
    bb_http_client_result_t res;
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_post(sess, "https://example.com/path", NULL, 0, NULL, &res));
    TEST_ASSERT_EQUAL_INT(-0x7200, res.tls_error_code);
    bb_http_client_session_close(sess);
    bb_http_client_clear_mock();
}

void test_bb_http_client_session_post_tls_error_code_zero_default(void)
{
    bb_http_client_session_t sess;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "https://example.com", &sess));
    // No tls mock set — default is 0.
    bb_http_client_session_set_mock_status(200);
    bb_http_client_result_t res;
    memset(&res, 0xff, sizeof(res));  // Fill with non-zero to detect if 0 is set
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_post(sess, "https://example.com/path", NULL, 0, NULL, &res));
    TEST_ASSERT_EQUAL_INT(0, res.tls_error_code);
    bb_http_client_session_close(sess);
    bb_http_client_clear_mock();
}

// -------------------------------------------------------------------------
// session_set_header / find_header / header_at / header_count
//
// B1-...: the header-capture API lost its only direct test when the
// bb_pub/bb_sink_* cluster was deleted (it was only ever covered
// transitively via bb_sink_http's session usage). Still has live ESP-IDF
// callers (any session-based sink attaching auth/content headers) — direct
// tests restore real coverage.
// -------------------------------------------------------------------------

void test_bb_http_client_session_set_header_null_session_returns_invalid_arg(void)
{
    reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_set_header(NULL, "X-A", "1"));
}

void test_bb_http_client_session_set_header_null_name_returns_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_set_header(s, NULL, "1"));
    bb_http_client_session_close(s);
}

void test_bb_http_client_session_set_header_null_value_returns_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_http_client_session_set_header(s, "X-A", NULL));
    bb_http_client_session_close(s);
}

void test_bb_http_client_session_set_header_appends_new_entry(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));

    TEST_ASSERT_EQUAL(0, bb_http_client_session_header_count());
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_set_header(s, "Authorization", "Bearer tok"));
    TEST_ASSERT_EQUAL(1, bb_http_client_session_header_count());

    bb_http_client_header_record_t rec = bb_http_client_session_header_at(0);
    TEST_ASSERT_EQUAL_STRING("Authorization", rec.name);
    TEST_ASSERT_EQUAL_STRING("Bearer tok", rec.value);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_set_header_replaces_existing_entry(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));

    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_set_header(s, "X-A", "first"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_set_header(s, "X-A", "second"));
    // Replacing must not append a second entry.
    TEST_ASSERT_EQUAL(1, bb_http_client_session_header_count());

    bb_http_client_header_record_t rec = bb_http_client_session_header_at(0);
    TEST_ASSERT_EQUAL_STRING("second", rec.value);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_header_at_out_of_range_returns_zeroed(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    bb_http_client_header_record_t rec = bb_http_client_session_header_at(0);
    TEST_ASSERT_EQUAL_STRING("", rec.name);
    TEST_ASSERT_EQUAL_STRING("", rec.value);
    bb_http_client_session_close(s);
}

void test_bb_http_client_session_find_header_found(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    bb_http_client_session_set_header(s, "X-A", "1");
    bb_http_client_session_set_header(s, "X-B", "2");

    bb_http_client_header_record_t rec = bb_http_client_session_find_header("X-B");
    TEST_ASSERT_EQUAL_STRING("X-B", rec.name);
    TEST_ASSERT_EQUAL_STRING("2", rec.value);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_find_header_not_found_returns_zeroed(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    bb_http_client_session_set_header(s, "X-A", "1");

    bb_http_client_header_record_t rec = bb_http_client_session_find_header("X-Missing");
    TEST_ASSERT_EQUAL_STRING("", rec.name);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_find_header_null_name_returns_zeroed(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    bb_http_client_session_set_header(s, "X-A", "1");

    bb_http_client_header_record_t rec = bb_http_client_session_find_header(NULL);
    TEST_ASSERT_EQUAL_STRING("", rec.name);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_open_resets_header_capture(void)
{
    reset();
    bb_http_client_session_t s1 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s1));
    bb_http_client_session_set_header(s1, "X-A", "1");
    TEST_ASSERT_EQUAL(1, bb_http_client_session_header_count());
    bb_http_client_session_close(s1);

    // A fresh session_open must reset the header capture table.
    bb_http_client_session_t s2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s2));
    TEST_ASSERT_EQUAL(0, bb_http_client_session_header_count());
    bb_http_client_session_close(s2);
}

// -------------------------------------------------------------------------
// session_open_count / session_last_keep_alive
// -------------------------------------------------------------------------

void test_bb_http_client_session_open_count_increments_per_open(void)
{
    reset();
    TEST_ASSERT_EQUAL(0, bb_http_client_session_open_count());

    bb_http_client_session_t s1 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s1));
    TEST_ASSERT_EQUAL(1, bb_http_client_session_open_count());

    bb_http_client_session_t s2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s2));
    TEST_ASSERT_EQUAL(2, bb_http_client_session_open_count());

    bb_http_client_session_close(s1);
    bb_http_client_session_close(s2);
}

void test_bb_http_client_session_last_keep_alive_false_when_cfg_null(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    TEST_ASSERT_FALSE(bb_http_client_session_last_keep_alive());
    bb_http_client_session_close(s);
}

void test_bb_http_client_session_last_keep_alive_true_when_cfg_sets_it(void)
{
    reset();
    bb_http_client_cfg_t cfg = {0};
    cfg.keep_alive = true;
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(&cfg, "http://example.com", &s));
    TEST_ASSERT_TRUE(bb_http_client_session_last_keep_alive());
    bb_http_client_session_close(s);
}

void test_bb_http_client_session_last_keep_alive_false_when_cfg_clears_it(void)
{
    reset();
    bb_http_client_cfg_t cfg = {0};
    cfg.keep_alive = false;
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(&cfg, "http://example.com", &s));
    TEST_ASSERT_FALSE(bb_http_client_session_last_keep_alive());
    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// bb_http_client_host_set_session_calloc
// -------------------------------------------------------------------------

static int  s_alloc_calls = 0;
static void *counting_calloc(size_t n, size_t sz)
{
    s_alloc_calls++;
    return calloc(n, sz);
}

void test_bb_http_client_host_set_session_calloc_overrides_allocator(void)
{
    reset();
    s_alloc_calls = 0;
    bb_http_client_host_set_session_calloc(counting_calloc);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    TEST_ASSERT_EQUAL(1, s_alloc_calls);

    bb_http_client_session_close(s);
    bb_http_client_host_set_session_calloc(NULL);  // revert
}

void test_bb_http_client_host_set_session_calloc_null_reverts_to_real_calloc(void)
{
    reset();
    bb_http_client_host_set_session_calloc(counting_calloc);
    bb_http_client_host_set_session_calloc(NULL);

    s_alloc_calls = 0;
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    // counting_calloc must NOT have been invoked — the override was reverted.
    TEST_ASSERT_EQUAL(0, s_alloc_calls);
    TEST_ASSERT_NOT_NULL(s);

    bb_http_client_session_close(s);
}

// -------------------------------------------------------------------------
// Per-session health (B1-1041) — deterministic single-threaded coverage of
// bb_http_client_session_health_fill() driven via the existing mock knobs
// (session_set_mock_status / session_set_mock_transport_error /
// session_set_mock_tls_error_code). No pthread races — see
// bb_http_client.h's reporting policy for the contract under test.
// -------------------------------------------------------------------------

void test_bb_http_client_session_health_fill_null_args_return_invalid_arg(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_client_session_health_fill(s, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_client_session_health_fill(NULL, &snap));
    bb_http_client_session_close(s);
}

void test_bb_http_client_session_health_fill_fresh_session_is_zeroed(void)
{
    reset();
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_health_fill(s, &snap));
    TEST_ASSERT_FALSE(snap.connected);
    TEST_ASSERT_EQUAL_INT64(0, snap.last_ok_ms);
    TEST_ASSERT_EQUAL_UINT64(0, snap.fail_count);
    TEST_ASSERT_EQUAL_INT64(0, snap.tls_error_code);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_2xx_sets_connected_and_stamps_last_ok_ms(void)
{
    reset();
    bb_http_client_test_set_mock_time_ms64(1000);
    bb_http_client_session_set_mock_status(200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub", "{}", 2, NULL, &r));

    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_health_fill(s, &snap));
    TEST_ASSERT_TRUE(snap.connected);
    TEST_ASSERT_EQUAL_INT64(1000, snap.last_ok_ms);
    TEST_ASSERT_EQUAL_UINT64(0, snap.fail_count);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_transport_error_sets_disconnected_and_bumps_fail_count(void)
{
    reset();
    bb_http_client_test_set_mock_time_ms64(2000);
    bb_http_client_session_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
        bb_http_client_session_post(s, "http://example.com/pub", "{}", 2, NULL, &r));

    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_health_fill(s, &snap));
    TEST_ASSERT_FALSE(snap.connected);
    TEST_ASSERT_EQUAL_UINT64(1, snap.fail_count);
    // A transport failure never reaches the clock stamp -- last_ok_ms stays
    // at its zeroed default.
    TEST_ASSERT_EQUAL_INT64(0, snap.last_ok_ms);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_4xx_does_not_bump_fail_count(void)
{
    reset();
    bb_http_client_test_set_mock_time_ms64(3000);
    bb_http_client_session_set_mock_status(404);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r;
    // The host mock reports a completed round trip regardless of status
    // code (unlike the ESP-IDF backend, which additionally maps a non-2xx
    // status to BB_ERR_INVALID_STATE) -- health tracking only cares about
    // the outcome bb_http_client_result_t carries, so BB_OK here is correct
    // for this backend.
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub", "{}", 2, NULL, &r));

    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_health_fill(s, &snap));
    // A 4xx is a valid server response, not a transport failure: the round
    // trip completed, so connected/last_ok_ms are still reported, but
    // fail_count is NOT bumped.
    TEST_ASSERT_TRUE(snap.connected);
    TEST_ASSERT_EQUAL_INT64(3000, snap.last_ok_ms);
    TEST_ASSERT_EQUAL_UINT64(0, snap.fail_count);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_post_5xx_bumps_fail_count(void)
{
    reset();
    bb_http_client_test_set_mock_time_ms64(4000);
    bb_http_client_session_set_mock_status(503);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));

    bb_http_client_result_t r;
    // See the 4xx test above: the host mock reports BB_OK for any
    // transport-succeeded status code.
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_client_session_post(s, "http://example.com/pub", "{}", 2, NULL, &r));

    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_health_fill(s, &snap));
    // A 5xx is a server error even though the transport itself succeeded --
    // the round trip completed (connected/last_ok_ms report as usual) AND
    // fail_count is bumped.
    TEST_ASSERT_TRUE(snap.connected);
    TEST_ASSERT_EQUAL_INT64(4000, snap.last_ok_ms);
    TEST_ASSERT_EQUAL_UINT64(1, snap.fail_count);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_health_tls_error_code_set_on_failure(void)
{
    reset();
    bb_http_client_session_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_http_client_session_set_mock_tls_error_code(-0x7200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "https://example.com", &s));

    bb_http_client_result_t r;
    bb_http_client_session_post(s, "https://example.com/pub", NULL, 0, NULL, &r);

    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_health_fill(s, &snap));
    TEST_ASSERT_EQUAL_INT64(-0x7200, snap.tls_error_code);

    bb_http_client_session_close(s);
}

void test_bb_http_client_session_health_tls_error_code_not_reset_on_later_success(void)
{
    reset();
    bb_http_client_session_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_http_client_session_set_mock_tls_error_code(-0x7200);

    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "https://example.com", &s));

    bb_http_client_result_t r;
    bb_http_client_session_post(s, "https://example.com/pub", NULL, 0, NULL, &r);

    // A later successful post carries tls_error_code=0 in its own result,
    // but health must NOT reset the last-reported nonzero value to 0.
    bb_http_client_session_set_mock_transport_error(BB_OK);
    bb_http_client_session_set_mock_tls_error_code(0);
    bb_http_client_session_set_mock_status(200);
    bb_http_client_session_post(s, "https://example.com/pub", NULL, 0, NULL, &r);

    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_health_fill(s, &snap));
    TEST_ASSERT_EQUAL_INT64(-0x7200, snap.tls_error_code);
    TEST_ASSERT_TRUE(snap.connected);

    bb_http_client_session_close(s);
}

// Note: bb_http_client_session_close() frees the session handle, so
// bb_http_client_priv_health_close()'s connected=false/fail_count-untouched
// contract (documented in bb_http_client.h) is not independently
// assertable through the public API after a close -- fill-after-close would
// read freed memory. The close() codepath itself is exercised for coverage
// by every session_close() call across this file (host + espidf backends
// both call it unconditionally before freeing).

// -------------------------------------------------------------------------
// bb_http_client_session_health_desc: the descriptor produces exactly the 4
// documented wire keys, in order (mirrors bb_tcp_client_health_desc's test).
// -------------------------------------------------------------------------

typedef struct {
    int         count;
    const char *keys[4];
} http_client_health_desc_capture_t;

static void http_health_capture_emit_bool(void *ctx, const char *key, bool v)
{
    (void)v;
    http_client_health_desc_capture_t *cap = (http_client_health_desc_capture_t *)ctx;
    cap->keys[cap->count++] = key;
}

static void http_health_capture_emit_i64(void *ctx, const char *key, int64_t v)
{
    (void)v;
    http_client_health_desc_capture_t *cap = (http_client_health_desc_capture_t *)ctx;
    cap->keys[cap->count++] = key;
}

static void http_health_capture_emit_u64(void *ctx, const char *key, uint64_t v)
{
    (void)v;
    http_client_health_desc_capture_t *cap = (http_client_health_desc_capture_t *)ctx;
    cap->keys[cap->count++] = key;
}

void test_bb_http_client_session_health_desc_walks_all_four_keys(void)
{
    reset();
    bb_http_client_session_set_mock_status(200);
    bb_http_client_session_t s = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_open(NULL, "http://example.com", &s));
    bb_http_client_result_t r;
    bb_http_client_session_post(s, "http://example.com/pub", "{}", 2, NULL, &r);

    bb_http_client_session_health_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_http_client_session_health_fill(s, &snap));

    http_client_health_desc_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_serialize_emit_t emit = {
        .format_id = BB_FORMAT_NONE,
        .ctx       = &cap,
        .emit_bool = http_health_capture_emit_bool,
        .emit_i64  = http_health_capture_emit_i64,
        .emit_u64  = http_health_capture_emit_u64,
    };
    bb_serialize_walk(&bb_http_client_session_health_desc, &snap, &emit);

    TEST_ASSERT_EQUAL_INT(4, cap.count);
    TEST_ASSERT_EQUAL_STRING("connected", cap.keys[0]);
    TEST_ASSERT_EQUAL_STRING("last_ok_ms", cap.keys[1]);
    TEST_ASSERT_EQUAL_STRING("fail_count", cap.keys[2]);
    TEST_ASSERT_EQUAL_STRING("tls_error_code", cap.keys[3]);

    bb_http_client_session_close(s);
}
