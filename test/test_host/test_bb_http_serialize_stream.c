// Host tests for bb_http_serialize_stream() -- the HTTP adapter over
// bb_serialize_json_stream_render() (B1-1077 PR-1). Driven against the host
// bb_http_resp_send_chunk backend via the same capture harness
// test_bb_http_json_obj_stream.c/test_route_fidelity.c use -- reused here,
// not hand-rolled a second time.

#include "unity.h"
#include "bb_http_serialize_stream.h"
#include "bb_http_host.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixture: flat scalars
// ---------------------------------------------------------------------------

typedef struct {
    int64_t a;
    bool    b;
} flat_snap_t;

static const bb_serialize_field_t s_flat_fields[] = {
    { .key = "a", .type = BB_TYPE_I64,  .offset = offsetof(flat_snap_t, a) },
    { .key = "b", .type = BB_TYPE_BOOL, .offset = offsetof(flat_snap_t, b) },
};

static const bb_serialize_desc_t s_flat_desc = {
    .type_name = "flat_snap_t",
    .fields = s_flat_fields,
    .n_fields = 2,
    .snap_size = sizeof(flat_snap_t),
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_http_request_t *s_req;
static bb_http_host_capture_t s_cap;

static void cap_begin(void) { bb_http_host_capture_begin(&s_req); }
static void cap_end(void)   { bb_http_host_capture_end(s_req, &s_cap); }
static void cap_free(void)  { bb_http_host_capture_free(&s_cap); }

// ---------------------------------------------------------------------------
// 1. Happy path: content-type set, body byte-identical to
// bb_serialize_json_render()'s output.
// ---------------------------------------------------------------------------

void test_http_serialize_stream_happy_path(void)
{
    flat_snap_t snap = { .a = 7, .b = false };

    char   bounded_buf[128];
    size_t bounded_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_render(&s_flat_desc, &snap,
                                                       bounded_buf, sizeof(bounded_buf), &bounded_len));

    cap_begin();
    bb_err_t err = bb_http_serialize_stream(s_req, &s_flat_desc, &snap);
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("application/json", s_cap.content_type);
    TEST_ASSERT_NOT_NULL(s_cap.body);
    TEST_ASSERT_EQUAL_STRING(bounded_buf, s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// 2. NULL-argument validation
// ---------------------------------------------------------------------------

void test_http_serialize_stream_null_req_invalid_arg(void)
{
    flat_snap_t snap = { 0 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_serialize_stream(NULL, &s_flat_desc, &snap));
}

void test_http_serialize_stream_null_desc_invalid_arg(void)
{
    flat_snap_t snap = { 0 };
    cap_begin();
    bb_err_t err = bb_http_serialize_stream(s_req, NULL, &snap);
    cap_end();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    cap_free();
}

void test_http_serialize_stream_null_snap_invalid_arg(void)
{
    cap_begin();
    bb_err_t err = bb_http_serialize_stream(s_req, &s_flat_desc, NULL);
    cap_end();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    cap_free();
}

// ---------------------------------------------------------------------------
// 3. set_type failure short-circuits before any chunk is sent.
// ---------------------------------------------------------------------------

void test_http_serialize_stream_set_type_fail_short_circuits(void)
{
    flat_snap_t snap = { .a = 1, .b = true };

    cap_begin();
    bb_http_host_force_set_type_fail(true);
    bb_err_t err = bb_http_serialize_stream(s_req, &s_flat_desc, &snap);
    bb_http_host_force_set_type_fail(false);
    cap_end();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_NULL(s_cap.body);  // nothing written
    cap_free();
}

// ---------------------------------------------------------------------------
// 4. Mid-stream send_chunk failure: returns the ORIGINAL bb_http_resp_
// send_chunk error (not bb_serialize_json's synthetic stream-abort code),
// and the chunked response is still finalized (terminator call attempted).
// ---------------------------------------------------------------------------

void test_http_serialize_stream_send_chunk_fail_propagates_original_error(void)
{
    flat_snap_t snap = { .a = 99, .b = true };

    cap_begin();
    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_serialize_stream(s_req, &s_flat_desc, &snap);
    bb_http_host_force_send_chunk_fail(false);
    size_t call_count = bb_http_host_send_chunk_call_count();
    cap_end();

    // The host backend's forced failure is BB_ERR_NO_SPACE -- distinct from
    // bb_serialize_json_stream_render()'s synthetic BB_ERR_INVALID_STATE
    // abort code, proving the real I/O error is what's surfaced.
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    // s_force_send_chunk_fail makes EVERY send_chunk call fail identically,
    // so the error code alone can't prove the terminator call was actually
    // attempted after the content flush failed (vs skipped once fc.failed
    // latched) -- this flat snap fits bb_serialize_json_stream_render()'s
    // internal flush buffer whole, so exactly ONE content flush happens
    // (bb_json_flush's tail flush), then bb_http_serialize_stream() ALWAYS
    // issues the terminator call regardless of fc.failed: 2 total calls.
    TEST_ASSERT_EQUAL_size_t(2, call_count);
    cap_free();
}
