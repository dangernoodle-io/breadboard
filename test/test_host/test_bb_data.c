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

// Registers a fake format under BB_FORMAT_JSON: REAL JSON render/parse fns
// (bb_serialize_json_render / bb_serialize_json_parse_bytes, the B1-1030
// composed adapter) -- exercises bb_data_apply()'s parse dispatch against
// real JSON bytes, not a hand-rolled stub. Test-isolated: resets the format
// registry first.
static void dt_register_format(void)
{
    static const bb_serialize_format_entry_t entry = {
        .render = bb_serialize_json_render,
        .parse  = bb_serialize_json_parse_bytes,
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

// ---------------------------------------------------------------------------
// bb_data_apply (B1-1022) -- the write-half mirror of bb_data_render, driven
// against real JSON parse/populate (dt_register_format() above).
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} dt_apply_snap_t;

static const bb_serialize_field_t s_dt_apply_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(dt_apply_snap_t, n) },
};

static const bb_serialize_desc_t s_dt_apply_desc = {
    .type_name = "dt_apply_snap_t", .fields = s_dt_apply_fields, .n_fields = 1,
    .snap_size = sizeof(dt_apply_snap_t),
};

// PATCH-seed gather: fills the whole struct from `ctx` (the "live" value),
// same fully-initializing contract every bb_data_gather_fn already carries.
static bb_err_t dt_apply_gather_live(void *dst, const bb_data_gather_args_t *args)
{
    ((dt_apply_snap_t *)dst)->n = *(int64_t *)args->ctx;
    return BB_OK;
}

static int64_t s_dt_apply_spy_n     = 0;
static int     s_dt_apply_spy_calls = 0;

static void dt_apply_spy_reset(void)
{
    s_dt_apply_spy_n     = 0;
    s_dt_apply_spy_calls = 0;
}

static bb_err_t dt_apply_spy_ok(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    s_dt_apply_spy_n = ((const dt_apply_snap_t *)snap)->n;
    s_dt_apply_spy_calls++;
    return BB_OK;
}

static bb_err_t dt_apply_spy_validation_fail(const void *snap, const bb_data_apply_args_t *args)
{
    (void)snap;
    (void)args;
    s_dt_apply_spy_calls++;
    return BB_ERR_VALIDATION;
}

static bb_err_t dt_apply_gather_fail(void *dst, const bb_data_gather_args_t *args)
{
    (void)dst;
    (void)args;
    return BB_ERR_INVALID_STATE;
}

// A descriptor bb_serialize_populate() itself pre-flight-rejects (a
// BB_TYPE_ARR field with max_items == 0) -- used to prove a populate
// rejection propagates from bb_data_apply() WITHOUT ever invoking apply().
typedef struct {
    bb_serialize_arr_t bad_arr;
} dt_apply_bad_snap_t;

static const bb_serialize_field_t s_dt_apply_bad_fields[] = {
    { .key = "bad", .type = BB_TYPE_ARR, .offset = offsetof(dt_apply_bad_snap_t, bad_arr),
      .elem_type = BB_TYPE_I64, .max_items = 0 },
};

static const bb_serialize_desc_t s_dt_apply_bad_desc = {
    .type_name = "dt_apply_bad_snap_t", .fields = s_dt_apply_bad_fields, .n_fields = 1,
    .snap_size = sizeof(dt_apply_bad_snap_t),
};

static bb_err_t dt_apply_bad_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    memset(dst, 0, sizeof(dt_apply_bad_snap_t));
    return BB_OK;
}

// Must comfortably fit bb_mem_arena's own header + the JSON parse adapter's
// token recorder + populate ctx + its default-capacity token pool (48 *
// sizeof(bb_serialize_json_tok_t) alone is 2304 bytes) -- see
// test_bb_serialize_json_parse.c's SCRATCH_CAP comment for the full layout.
#define DT_APPLY_PARSE_SCRATCH_CAP 4096
static char s_dt_apply_parse_scratch[DT_APPLY_PARSE_SCRATCH_CAP];

void test_bb_data_apply_post_zeroes_unset_fields(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    int64_t live_val = 42;
    bb_data_binding_t b = { .key = "dt.apply.post", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok, .ctx = &live_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { .n = -1 };
    const char *body = "{}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.post", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(1, s_dt_apply_spy_calls);
    TEST_ASSERT_EQUAL_INT64(0, s_dt_apply_spy_n);  // zero-seeded, not the "live" 42
}

void test_bb_data_apply_patch_preserves_unset_fields(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    int64_t live_val = 42;
    bb_data_binding_t b = { .key = "dt.apply.patch", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok, .ctx = &live_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { .n = -1 };
    const char *body = "{}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.patch", .mode = BB_DATA_APPLY_PATCH,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(1, s_dt_apply_spy_calls);
    TEST_ASSERT_EQUAL_INT64(42, s_dt_apply_spy_n);  // seeded from gather(), body never set "n"
}

void test_bb_data_apply_patch_body_field_overrides_seeded_value(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    int64_t live_val = 42;
    bb_data_binding_t b = { .key = "dt.apply.patch.override", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok, .ctx = &live_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { .n = -1 };
    const char *body = "{\"n\":7}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.patch.override", .mode = BB_DATA_APPLY_PATCH,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(1, s_dt_apply_spy_calls);
    TEST_ASSERT_EQUAL_INT64(7, s_dt_apply_spy_n);  // body's "n" wins over the seeded 42
}

void test_bb_data_apply_patch_seed_gather_failure_propagates(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.apply.patchgatherfail", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_fail, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { 0 };
    const char *body = "{\"n\":1}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.patchgatherfail", .mode = BB_DATA_APPLY_PATCH,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);  // apply() never reached: the seed gather() itself failed
}

void test_bb_data_apply_zero_body_len_skips_null_body_check(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.apply.zerobody", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { 0 };
    // body_len == 0 with a non-NULL body is NOT rejected by bb_data_apply()'s
    // own NULL-body check (it only fires when body_len > 0 -- proving that
    // short-circuit's "false" outcome executes too, not just its "true"
    // one) -- it proceeds to the parse fn, which itself rejects the empty
    // document.
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.zerobody", .mode = BB_DATA_APPLY_POST,
        .body = "", .body_len = 0,
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    bb_err_t rc = bb_data_apply(&req);
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);  // apply() never reached: the parse itself failed
}

void test_bb_data_apply_malformed_body_propagates_parse_err(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.apply.malformed", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { 0 };
    const char *body = "{not json";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.malformed", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    bb_err_t rc = bb_data_apply(&req);
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);  // apply() never reached
}

void test_bb_data_apply_populate_rejected_shape_propagates_before_apply(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.apply.badshape", .desc = &s_dt_apply_bad_desc,
                            .gather = dt_apply_bad_gather, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_bad_snap_t dst;
    const char *body = "{}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.badshape", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_apply(&req));  // populate's max_items==0 pre-flight reject
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);  // apply() never reached
}

void test_bb_data_apply_apply_validation_error_propagates(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.apply.reject", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_validation_fail };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { 0 };
    const char *body = "{\"n\":1}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.reject", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(1, s_dt_apply_spy_calls);  // apply() WAS reached this time, and rejected
}

void test_bb_data_apply_unknown_key_returns_not_found(void)
{
    dt_reset();
    dt_register_format();

    dt_apply_snap_t dst = { 0 };
    const char *body = "{}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.nope", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_apply(&req));
}

void test_bb_data_apply_apply_null_returns_unsupported(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    // .apply omitted -- egress-only binding.
    bb_data_binding_t b = { .key = "dt.apply.noapply", .desc = &s_dt_apply_desc, .gather = dt_apply_gather_live };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { 0 };
    const char *body = "{}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.noapply", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);
}

void test_bb_data_apply_unregistered_format_returns_unsupported(void)
{
    dt_reset();
    bb_serialize_format_test_reset();  // no format registered at all
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.apply.nofmt", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { 0 };
    const char *body = "{}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.nofmt", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);  // true no-op: gather/apply never invoked
}

void test_bb_data_apply_dst_scratch_too_small_returns_no_space(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.apply.smallscratch", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    char dst[1];  // smaller than s_dt_apply_desc.snap_size
    const char *body = "{}";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.smallscratch", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);
}

void test_bb_data_apply_null_args_return_invalid_arg(void)
{
    dt_reset();
    dt_register_format();

    bb_data_binding_t b = { .key = "dt.apply.nullargs", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { 0 };
    const char *body = "{}";

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_apply(NULL));

    bb_data_apply_req_t req_no_key = {
        .fmt = BB_FORMAT_JSON, .key = NULL, .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_apply(&req_no_key));

    bb_data_apply_req_t req_no_parse_scratch = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.nullargs", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = NULL, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_apply(&req_no_parse_scratch));

    bb_data_apply_req_t req_no_dst_scratch = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.nullargs", .mode = BB_DATA_APPLY_POST,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = NULL, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_apply(&req_no_dst_scratch));

    bb_data_apply_req_t req_body_len_no_body = {
        .fmt = BB_FORMAT_JSON, .key = "dt.apply.nullargs", .mode = BB_DATA_APPLY_POST,
        .body = NULL, .body_len = 2,
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_apply(&req_body_len_no_body));
}

// ---------------------------------------------------------------------------
// bb_data_parse() / bb_data_commit() -- the parse/commit split (bb_http_section
// PR). bb_data_apply() above is now a thin wrapper over these two; the tests
// above already pin its composed behavior end to end. These tests exercise
// the split calls directly, plus the ordering contract the split depends on:
// bb_data_commit()'s seed step (gather/memset) now runs AFTER
// bb_data_parse()'s decode, not before it (see bb_data_commit()'s doc).
// ---------------------------------------------------------------------------

static int s_dt_split_gather_calls = 0;

// A PATCH-seed gather that WOULD fail if ever invoked -- used to prove
// bb_data_commit() is never reached (and this hook never called) when
// bb_data_parse() itself already failed on a malformed body.
static bb_err_t dt_split_gather_would_fail(void *dst, const bb_data_gather_args_t *args)
{
    (void)dst;
    (void)args;
    s_dt_split_gather_calls++;
    return BB_ERR_INVALID_STATE;
}

void test_bb_data_parse_decodes_body_and_commit_applies(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    int64_t live_val = 42;
    bb_data_binding_t b = { .key = "dt.split.ok", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok, .ctx = &live_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    const char *body = "{\"n\":7}";
    bb_data_parse_req_t parse_req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.ok",
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    bb_data_parsed_t parsed;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_parse(&parse_req, &parsed));

    dt_apply_snap_t dst = { .n = -1 };
    bb_data_commit_req_t commit_req = {
        .mode = BB_DATA_APPLY_POST, .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_commit(&parsed, &commit_req));
    TEST_ASSERT_EQUAL(1, s_dt_apply_spy_calls);
    TEST_ASSERT_EQUAL_INT64(7, s_dt_apply_spy_n);
}

void test_bb_data_parse_null_args_return_invalid_arg(void)
{
    dt_reset();
    dt_register_format();

    bb_data_binding_t b = { .key = "dt.split.nullargs", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_data_parsed_t parsed;
    const char *body = "{}";

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_parse(NULL, &parsed));

    bb_data_parse_req_t req_no_key = {
        .fmt = BB_FORMAT_JSON, .key = NULL,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_parse(&req_no_key, &parsed));

    bb_data_parse_req_t req_no_scratch = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.nullargs",
        .body = body, .body_len = strlen(body),
        .parse_scratch = NULL, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_parse(&req_no_scratch, &parsed));

    bb_data_parse_req_t req_no_out = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.nullargs",
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_parse(&req_no_out, NULL));

    bb_data_parse_req_t req_body_len_no_body = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.nullargs",
        .body = NULL, .body_len = 2,
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_parse(&req_body_len_no_body, &parsed));
}

void test_bb_data_parse_unknown_key_returns_not_found(void)
{
    dt_reset();
    dt_register_format();

    bb_data_parsed_t parsed;
    const char *body = "{}";
    bb_data_parse_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.nope",
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_parse(&req, &parsed));
}

void test_bb_data_parse_apply_less_binding_returns_unsupported(void)
{
    dt_reset();
    dt_register_format();

    // .apply omitted -- egress-only binding.
    bb_data_binding_t b = { .key = "dt.split.noapply", .desc = &s_dt_apply_desc, .gather = dt_apply_gather_live };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_data_parsed_t parsed;
    const char *body = "{}";
    bb_data_parse_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.noapply",
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_parse(&req, &parsed));
}

void test_bb_data_parse_unregistered_format_returns_unsupported(void)
{
    dt_reset();
    bb_serialize_format_test_reset();  // no format registered at all

    bb_data_binding_t b = { .key = "dt.split.nofmt", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_data_parsed_t parsed;
    const char *body = "{}";
    bb_data_parse_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.nofmt",
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_parse(&req, &parsed));
}

void test_bb_data_parse_malformed_body_returns_parse_grammar(void)
{
    dt_reset();
    dt_register_format();

    bb_data_binding_t b = { .key = "dt.split.malformed", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_data_parsed_t parsed;
    const char *body = "{not json";
    bb_data_parse_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.malformed",
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    TEST_ASSERT_EQUAL(BB_ERR_PARSE_GRAMMAR, bb_data_parse(&req, &parsed));
}

void test_bb_data_commit_null_args_return_invalid_arg(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.commit.nullargs", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    const char *body = "{}";
    bb_data_parse_req_t parse_req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.commit.nullargs",
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    bb_data_parsed_t parsed;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_parse(&parse_req, &parsed));

    dt_apply_snap_t dst = { 0 };
    bb_data_commit_req_t commit_req = {
        .mode = BB_DATA_APPLY_POST, .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_commit(NULL, &commit_req));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_commit(&parsed, NULL));

    bb_data_parsed_t zero_parsed = { 0 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_commit(&zero_parsed, &commit_req));

    bb_data_commit_req_t commit_req_no_dst = {
        .mode = BB_DATA_APPLY_POST, .dst_scratch = NULL, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_commit(&parsed, &commit_req_no_dst));
}

void test_bb_data_commit_dst_scratch_too_small_returns_no_space(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();

    bb_data_binding_t b = { .key = "dt.commit.smallscratch", .desc = &s_dt_apply_desc,
                            .gather = dt_apply_gather_live, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    const char *body = "{}";
    bb_data_parse_req_t parse_req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.commit.smallscratch",
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
    };
    bb_data_parsed_t parsed;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_parse(&parse_req, &parsed));

    char dst[1];  // smaller than s_dt_apply_desc.snap_size
    bb_data_commit_req_t commit_req = {
        .mode = BB_DATA_APPLY_POST, .dst_scratch = dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_commit(&parsed, &commit_req));
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);
}

// THE ordering-contract test (B1-1022/bb_http_section PR, user-directed):
// PATCH mode + a MALFORMED body + a gather() that would fail if invoked.
// Proves bb_data_apply()'s composed call order is decode-before-seed
// (bb_data_parse() runs, and fails, before bb_data_commit()'s seed step
// ever calls gather()) -- the one interaction no pre-split test exercised
// (see the bb_http_section-PR audit that surfaced this gap: no existing
// test combined PATCH mode + an undecodable body + a failing gather).
//
// REVERT-PROOF: if bb_data_commit()'s seed step were moved back to run
// BEFORE bb_data_parse()'s decode (the pre-split source order), this test
// goes RED two ways: (1) the returned code becomes BB_ERR_INVALID_STATE
// (dt_split_gather_would_fail's return) instead of BB_ERR_PARSE_GRAMMAR,
// and (2) s_dt_split_gather_calls becomes 1 instead of 0. Manually verified
// by temporarily calling the old seed-then-parse order in bb_data_apply()
// -- see this PR's report for the observed red/green.
void test_bb_data_apply_patch_malformed_body_never_invokes_seed_gather(void)
{
    dt_reset();
    dt_register_format();
    dt_apply_spy_reset();
    s_dt_split_gather_calls = 0;

    bb_data_binding_t b = { .key = "dt.split.patchmalformed", .desc = &s_dt_apply_desc,
                            .gather = dt_split_gather_would_fail, .apply = dt_apply_spy_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dt_apply_snap_t dst = { 0 };
    const char *body = "{not json";
    bb_data_apply_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = "dt.split.patchmalformed", .mode = BB_DATA_APPLY_PATCH,
        .body = body, .body_len = strlen(body),
        .parse_scratch = s_dt_apply_parse_scratch, .parse_scratch_cap = DT_APPLY_PARSE_SCRATCH_CAP,
        .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    TEST_ASSERT_EQUAL(BB_ERR_PARSE_GRAMMAR, bb_data_apply(&req));
    TEST_ASSERT_EQUAL(0, s_dt_split_gather_calls);  // seed gather() never reached: decode failed first
    TEST_ASSERT_EQUAL(0, s_dt_apply_spy_calls);
}
