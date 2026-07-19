// Host tests for bb_data's core binding table (B1-832): bb_data_bind() /
// bb_data_render(). EGRESS ONLY -- ingress/populate is deferred (see
// bb_data.h). Uses a trivial fake format entry (registered/reset via the
// real bb_serialize_format API) plus the real bb_meminfo_heap_snap_desc
// fixture where a real descriptor adds value.

#include "unity.h"

#include "bb_data.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"

#include "../../components/bb_meminfo/include/bb_meminfo_heap_snap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixture: a tiny snapshot type + fake format entry.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} dt_snap_t;

static const bb_serialize_field_t s_dt_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(dt_snap_t, n) },
};

static const bb_serialize_desc_t s_dt_desc = {
    .type_name = "dt_snap_t",
    .fields    = s_dt_fields,
    .n_fields  = 1,
    .snap_size = sizeof(dt_snap_t),
};

static bb_err_t dt_gather_ok(void *dst, const bb_data_gather_args_t *args)
{
    ((dt_snap_t *)dst)->n = *(int64_t *)args->ctx;
    return BB_OK;
}

static bb_err_t dt_gather_fail(void *dst, const bb_data_gather_args_t *args)
{
    (void)dst;
    (void)args;
    return BB_ERR_INVALID_STATE;
}

// Adapter: bb_meminfo_heap_snap_fill() takes a single out-param, not the
// (dst, args) shape bb_data_gather_fn requires -- wrap rather than cast the
// fn pointer across a mismatched signature.
static bb_err_t dt_gather_meminfo(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_meminfo_heap_snap_fill((bb_meminfo_heap_snap_t *)dst);
}

// Asserts the request-scoped query carried through to the gather hook
// matches the expected "type" value, then fills the snapshot like
// dt_gather_ok().
static const char *s_dt_gather_query_expected_type = NULL;

static bb_err_t dt_gather_query(void *dst, const bb_data_gather_args_t *args)
{
    TEST_ASSERT_EQUAL_STRING(s_dt_gather_query_expected_type,
                              bb_serialize_query_get(args->query, "type"));
    ((dt_snap_t *)dst)->n = *(int64_t *)args->ctx;
    return BB_OK;
}

// Registers a fake format under BB_FORMAT_JSON: a REAL JSON renderer
// (bb_serialize_json_render). Test-isolated: resets the format registry
// first.
static void dt_register_format(void)
{
    static const bb_serialize_format_entry_t entry = {
        .render = bb_serialize_json_render,
        .parse  = NULL,
    };
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry));
}

static void dt_reset(void)
{
    bb_data_test_reset();
}

// ---------------------------------------------------------------------------
// bb_data_bind
// ---------------------------------------------------------------------------

void test_bb_data_bind_success(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.bind", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));
}

void test_bb_data_bind_override_replaces_binding(void)
{
    dt_reset();
    int64_t ctx_a = 1;
    int64_t ctx_b = 2;
    bb_data_binding_t a = { .key = "dt.override", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_a };
    bb_data_binding_t b = { .key = "dt.override", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_b };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&a));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_register_format();
    char   scratch[sizeof(dt_snap_t)];
    char   buf[64];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.override", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render(&req));
    TEST_ASSERT_EQUAL_STRING("{\"n\":2}", buf);
}

void test_bb_data_bind_null_binding_returns_invalid_arg(void)
{
    dt_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(NULL));
}

void test_bb_data_bind_null_key_returns_invalid_arg(void)
{
    dt_reset();
    bb_data_binding_t b = { .key = NULL, .desc = &s_dt_desc, .gather = dt_gather_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

void test_bb_data_bind_null_desc_returns_invalid_arg(void)
{
    dt_reset();
    bb_data_binding_t b = { .key = "dt.nodesc", .desc = NULL, .gather = dt_gather_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

void test_bb_data_bind_useless_binding_rejected(void)
{
    dt_reset();
    bb_data_binding_t b = { .key = "dt.useless", .desc = &s_dt_desc, .gather = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

void test_bb_data_bind_key_max_length_boundary(void)
{
    dt_reset();

    char key_ok[BB_DATA_KEY_MAX];
    memset(key_ok, 'k', sizeof(key_ok) - 1);
    key_ok[sizeof(key_ok) - 1] = '\0';  // strlen == BB_DATA_KEY_MAX - 1, fits
    bb_data_binding_t ok = { .key = key_ok, .desc = &s_dt_desc, .gather = dt_gather_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&ok));

    char key_over[BB_DATA_KEY_MAX + 1];
    memset(key_over, 'k', sizeof(key_over) - 1);
    key_over[sizeof(key_over) - 1] = '\0';  // strlen == BB_DATA_KEY_MAX, over
    bb_data_binding_t over = { .key = key_over, .desc = &s_dt_desc, .gather = dt_gather_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&over));
}

void test_bb_data_bind_capacity_full_returns_no_space(void)
{
    dt_reset();
    char keys[BB_DATA_MAX_BINDINGS + 1][32];
    for (int i = 0; i < BB_DATA_MAX_BINDINGS; i++) {
        snprintf(keys[i], sizeof(keys[i]), "dt.cap.%d", i);
        bb_data_binding_t b = { .key = keys[i], .desc = &s_dt_desc, .gather = dt_gather_ok };
        TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));
    }

    snprintf(keys[BB_DATA_MAX_BINDINGS], sizeof(keys[BB_DATA_MAX_BINDINGS]), "dt.cap.%d", BB_DATA_MAX_BINDINGS);
    bb_data_binding_t overflow = { .key = keys[BB_DATA_MAX_BINDINGS], .desc = &s_dt_desc, .gather = dt_gather_ok };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_bind(&overflow));

    // Re-binding an ALREADY-bound key (override) still succeeds even with
    // the table full.
    bb_data_binding_t rebind = { .key = keys[0], .desc = &s_dt_desc, .gather = dt_gather_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&rebind));
}

// ---------------------------------------------------------------------------
// bb_data_render
// ---------------------------------------------------------------------------

void test_bb_data_render_happy_path_with_meminfo_fixture(void)
{
    dt_reset();
    dt_register_format();

    bb_data_binding_t b = { .key = "dt.meminfo", .desc = &bb_meminfo_heap_snap_desc,
                            .gather = dt_gather_meminfo };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_meminfo_heap_snap_t scratch;
    char                   buf[1024];
    size_t                 out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.meminfo", .query = NULL,
        .scratch = &scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render(&req));
    TEST_ASSERT_TRUE(out_len > 0);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

void test_bb_data_render_unbound_key_returns_not_found(void)
{
    dt_reset();
    dt_register_format();

    char   scratch[sizeof(dt_snap_t)];
    char   buf[64];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.nope", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_render(&req));
}

void test_bb_data_render_unregistered_format_returns_unsupported(void)
{
    dt_reset();
    bb_serialize_format_test_reset();  // no format registered at all

    int64_t ctx_val = 5;
    bb_data_binding_t b = { .key = "dt.nofmt", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    char   scratch[sizeof(dt_snap_t)];
    char   buf[64];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.nofmt", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_render(&req));
}

void test_bb_data_render_gather_failure_propagates(void)
{
    dt_reset();
    dt_register_format();

    bb_data_binding_t b = { .key = "dt.gatherfail", .desc = &s_dt_desc, .gather = dt_gather_fail };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    char   scratch[sizeof(dt_snap_t)];
    char   buf[64];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.gatherfail", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_data_render(&req));
}

void test_bb_data_render_buf_too_small_returns_no_space(void)
{
    dt_reset();
    dt_register_format();

    int64_t ctx_val = 123456789;
    bb_data_binding_t b = { .key = "dt.overflow", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    char   scratch[sizeof(dt_snap_t)];
    char   buf[4];  // {"n": already overflows a 4-byte buffer
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.overflow", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_render(&req));
}

void test_bb_data_render_scratch_too_small_returns_no_space(void)
{
    dt_reset();
    dt_register_format();

    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.scratch", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    char   scratch[1];  // smaller than s_dt_desc.snap_size (sizeof(dt_snap_t))
    char   buf[64];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.scratch", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_render(&req));
}

void test_bb_data_render_unsupported_format_skips_gather(void)
{
    dt_reset();
    bb_serialize_format_test_reset();  // no format registered at all

    // dt_gather_fail always errors -- if bb_data_render() invoked it before
    // checking for a registered renderer, this would surface
    // BB_ERR_INVALID_STATE instead. Proves the renderer lookup happens
    // BEFORE gather() so an unsupported format is a true no-op (gather never
    // runs).
    bb_data_binding_t b = { .key = "dt.noop", .desc = &s_dt_desc, .gather = dt_gather_fail };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    char   scratch[sizeof(dt_snap_t)];
    char   buf[64];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.noop", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_render(&req));
}

// A render carrying a non-NULL query is forwarded to the gather hook
// byte-for-byte via bb_data_gather_args_t.query -- dt_gather_query() asserts
// it inline.
void test_bb_data_render_forwards_query_to_gather(void)
{
    dt_reset();
    dt_register_format();

    int64_t ctx_val = 7;
    bb_data_binding_t b = { .key = "dt.query", .desc = &s_dt_desc, .gather = dt_gather_query, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_serialize_query_t query = {
        .params = { { .key = "type", .value = "raw" } },
        .count  = 1,
    };
    s_dt_gather_query_expected_type = "raw";

    char   scratch[sizeof(dt_snap_t)];
    char   buf[64];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.query", .query = &query,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render(&req));
    TEST_ASSERT_EQUAL_STRING("{\"n\":7}", buf);
}

// A NULL query (the pre-existing back-compat shape) still renders
// successfully -- the gather hook observes args->query == NULL.
void test_bb_data_render_null_query_still_works(void)
{
    dt_reset();
    dt_register_format();

    int64_t ctx_val = 9;
    bb_data_binding_t b = { .key = "dt.noquery", .desc = &s_dt_desc, .gather = dt_gather_query, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dt_gather_query_expected_type = NULL;

    char   scratch[sizeof(dt_snap_t)];
    char   buf[64];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.noquery", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render(&req));
    TEST_ASSERT_EQUAL_STRING("{\"n\":9}", buf);
}

// ---------------------------------------------------------------------------
// replay_kind (B1-1032) -- STORED only, not yet consumed by any broadcaster.
// ---------------------------------------------------------------------------

void test_bb_data_bind_default_replay_kind_is_state(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    // replay_kind omitted from the struct literal -- must default to
    // BB_DATA_STATE (0), matching today's fresh-render-only behavior.
    bb_data_binding_t b = { .key = "dt.replay.default", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_data_replay_kind_t kind;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_binding_replay_kind("dt.replay.default", &kind));
    TEST_ASSERT_EQUAL(BB_DATA_STATE, kind);
}

void test_bb_data_bind_explicit_event_replay_kind_round_trips(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.replay.event", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val,
                            .replay_kind = BB_DATA_EVENT };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_data_replay_kind_t kind;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_binding_replay_kind("dt.replay.event", &kind));
    TEST_ASSERT_EQUAL(BB_DATA_EVENT, kind);
}

void test_bb_data_binding_replay_kind_unbound_key_returns_not_found(void)
{
    dt_reset();
    bb_data_replay_kind_t kind;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_binding_replay_kind("dt.replay.nope", &kind));
}

void test_bb_data_binding_replay_kind_null_args_return_invalid_arg(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.replay.nullargs", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_data_replay_kind_t kind;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_binding_replay_kind(NULL, &kind));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_binding_replay_kind("dt.replay.nullargs", NULL));
}

// ---------------------------------------------------------------------------
// generation (touch/generation) -- STORED coherence counter, no consumer yet.
// ---------------------------------------------------------------------------

void test_bb_data_generation_fresh_binding_is_zero(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.gen.fresh", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    uint32_t gen = 12345;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation("dt.gen.fresh", &gen));
    TEST_ASSERT_EQUAL_UINT32(0, gen);
}

void test_bb_data_touch_bumps_generation_by_one(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.gen.bump", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    TEST_ASSERT_EQUAL(BB_OK, bb_data_touch("dt.gen.bump"));

    uint32_t gen = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation("dt.gen.bump", &gen));
    TEST_ASSERT_EQUAL_UINT32(1, gen);
}

void test_bb_data_touch_repeated_calls_are_monotonic(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.gen.mono", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    for (uint32_t i = 1; i <= 5; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_data_touch("dt.gen.mono"));
        uint32_t gen = 0;
        TEST_ASSERT_EQUAL(BB_OK, bb_data_generation("dt.gen.mono", &gen));
        TEST_ASSERT_EQUAL_UINT32(i, gen);
    }
}

void test_bb_data_touch_independent_per_key(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    bb_data_binding_t a = { .key = "dt.gen.a", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    bb_data_binding_t b = { .key = "dt.gen.b", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&a));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    TEST_ASSERT_EQUAL(BB_OK, bb_data_touch("dt.gen.a"));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_touch("dt.gen.a"));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_touch("dt.gen.b"));

    uint32_t gen_a = 0, gen_b = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation("dt.gen.a", &gen_a));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation("dt.gen.b", &gen_b));
    TEST_ASSERT_EQUAL_UINT32(2, gen_a);
    TEST_ASSERT_EQUAL_UINT32(1, gen_b);
}

void test_bb_data_touch_unbound_key_returns_not_found(void)
{
    dt_reset();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_touch("dt.gen.nope"));
}

void test_bb_data_generation_unbound_key_returns_not_found(void)
{
    dt_reset();
    uint32_t gen = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_generation("dt.gen.nope", &gen));
}

void test_bb_data_touch_null_key_returns_invalid_arg(void)
{
    dt_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_touch(NULL));
}

void test_bb_data_generation_null_args_return_invalid_arg(void)
{
    dt_reset();
    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.gen.nullargs", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    uint32_t gen = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_generation(NULL, &gen));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_generation("dt.gen.nullargs", NULL));
}

void test_bb_data_render_null_args_return_invalid_arg(void)
{
    dt_reset();
    dt_register_format();

    int64_t ctx_val = 1;
    bb_data_binding_t b = { .key = "dt.nullargs", .desc = &s_dt_desc, .gather = dt_gather_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    char   scratch[sizeof(dt_snap_t)];
    char   buf[64];
    size_t out_len = 0;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render(NULL));

    bb_data_render_req_t req_no_key = {
        .fmt = BB_FORMAT_JSON, .key = NULL, .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render(&req_no_key));

    bb_data_render_req_t req_no_scratch = {
        .fmt = BB_FORMAT_JSON, .key = "dt.nullargs", .query = NULL,
        .scratch = NULL, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render(&req_no_scratch));

    bb_data_render_req_t req_no_buf = {
        .fmt = BB_FORMAT_JSON, .key = "dt.nullargs", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = NULL, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render(&req_no_buf));

    bb_data_render_req_t req_no_out_len = {
        .fmt = BB_FORMAT_JSON, .key = "dt.nullargs", .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = NULL,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render(&req_no_out_len));
}
