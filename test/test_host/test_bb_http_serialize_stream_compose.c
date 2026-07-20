// Host tests for bb_http_serialize_stream_compose() -- the HTTP adapter over
// bb_serialize_json_stream_compose_render() (B1-1097 PR-2). Driven against
// the host bb_http_resp_send_chunk backend, same capture harness as
// test_bb_http_serialize_stream.c.

#include "unity.h"
#include "bb_http_serialize_stream.h"
#include "bb_http_host.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

typedef struct {
    int64_t x;
} x_snap_t;

static const bb_serialize_field_t s_x_fields[] = {
    { .key = "x", .type = BB_TYPE_I64, .offset = offsetof(x_snap_t, x) },
};

static const bb_serialize_desc_t s_x_desc = {
    .type_name = "x_snap_t",
    .fields = s_x_fields,
    .n_fields = 1,
    .snap_size = sizeof(x_snap_t),
};

typedef struct {
    bool y;
} y_snap_t;

static const bb_serialize_field_t s_y_fields[] = {
    { .key = "y", .type = BB_TYPE_BOOL, .offset = offsetof(y_snap_t, y) },
};

static const bb_serialize_desc_t s_y_desc = {
    .type_name = "y_snap_t",
    .fields = s_y_fields,
    .n_fields = 1,
    .snap_size = sizeof(y_snap_t),
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
// 1. Happy path, single OBJECT group: content-type set, body byte-identical
// to bb_serialize_json_stream_compose_render()'s own captured output.
// ---------------------------------------------------------------------------

void test_http_serialize_stream_compose_happy_path(void)
{
    x_snap_t xs = { .x = 10 };
    y_snap_t ys = { .y = true };
    const bb_serialize_compose_entry_t entries[] = {
        { .name = "alpha", .desc = &s_x_desc, .snap = &xs },
        { .name = "beta",  .desc = &s_y_desc, .snap = &ys },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = entries, .n = 2, .shape = BB_SERIALIZE_COMPOSE_OBJECT },
    };

    cap_begin();
    bb_err_t err = bb_http_serialize_stream_compose(s_req, groups, 1);
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("application/json", s_cap.content_type);
    TEST_ASSERT_NOT_NULL(s_cap.body);
    TEST_ASSERT_EQUAL_STRING("{\"alpha\":{\"x\":10},\"beta\":{\"y\":true}}", s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// 2. Mixed-shape document over the HTTP bridge -- a RAW group followed by an
// OBJECT group, the same document shape /api/health needs (PR-5), proving
// the bridge (not just the JSON-level entry point) carries the mix through.
// ---------------------------------------------------------------------------

void test_http_serialize_stream_compose_mixed_shape_groups(void)
{
    x_snap_t xs = { .x = 1 };
    const bb_serialize_compose_entry_t root_entries[] = {
        { .desc = &s_x_desc, .snap = &xs },
    };
    y_snap_t ys = { .y = true };
    const bb_serialize_compose_entry_t section_entries[] = {
        { .name = "beta", .desc = &s_y_desc, .snap = &ys },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = root_entries,    .n = 1, .shape = BB_SERIALIZE_COMPOSE_RAW },
        { .entries = section_entries, .n = 1, .shape = BB_SERIALIZE_COMPOSE_OBJECT },
    };

    cap_begin();
    bb_err_t err = bb_http_serialize_stream_compose(s_req, groups, 2);
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("{\"x\":1,\"beta\":{\"y\":true}}", s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// 3. NULL/argument validation
// ---------------------------------------------------------------------------

void test_http_serialize_stream_compose_null_req_invalid_arg(void)
{
    x_snap_t xs = { .x = 1 };
    const bb_serialize_compose_entry_t entries[] = {
        { .desc = &s_x_desc, .snap = &xs },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = entries, .n = 1, .shape = BB_SERIALIZE_COMPOSE_RAW },
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_serialize_stream_compose(NULL, groups, 1));
}

void test_http_serialize_stream_compose_null_groups_nonzero_n_invalid_arg(void)
{
    cap_begin();
    bb_err_t err = bb_http_serialize_stream_compose(s_req, NULL, 1);
    cap_end();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    cap_free();
}

void test_http_serialize_stream_compose_null_groups_zero_n_ok(void)
{
    cap_begin();
    bb_err_t err = bb_http_serialize_stream_compose(s_req, NULL, 0);
    cap_end();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("{}", s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// 4. set_type failure short-circuits before any chunk is sent.
// ---------------------------------------------------------------------------

void test_http_serialize_stream_compose_set_type_fail_short_circuits(void)
{
    x_snap_t xs = { .x = 1 };
    const bb_serialize_compose_entry_t entries[] = {
        { .desc = &s_x_desc, .snap = &xs },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = entries, .n = 1, .shape = BB_SERIALIZE_COMPOSE_RAW },
    };

    cap_begin();
    bb_http_host_force_set_type_fail(true);
    bb_err_t err = bb_http_serialize_stream_compose(s_req, groups, 1);
    bb_http_host_force_set_type_fail(false);
    cap_end();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_NULL(s_cap.body);  // nothing written
    cap_free();
}

// ---------------------------------------------------------------------------
// 5. Mid-stream send_chunk failure: returns the ORIGINAL bb_http_resp_
// send_chunk error, and the chunked response is still finalized (terminator
// call attempted).
// ---------------------------------------------------------------------------

void test_http_serialize_stream_compose_send_chunk_fail_propagates_original_error(void)
{
    x_snap_t xs = { .x = 99 };
    const bb_serialize_compose_entry_t entries[] = {
        { .desc = &s_x_desc, .snap = &xs },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = entries, .n = 1, .shape = BB_SERIALIZE_COMPOSE_RAW },
    };

    cap_begin();
    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_serialize_stream_compose(s_req, groups, 1);
    bb_http_host_force_send_chunk_fail(false);
    size_t call_count = bb_http_host_send_chunk_call_count();
    cap_end();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    // One content flush (fits the internal flush buffer whole) + one
    // terminator call, always issued regardless of the content flush's
    // failure -- same shape as bb_http_serialize_stream()'s own test.
    TEST_ASSERT_EQUAL_size_t(2, call_count);
    cap_free();
}
