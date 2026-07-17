// Host tests for bb_data's core binding table (B1-832): bb_data_bind() /
// bb_data_render(). EGRESS ONLY -- ingress/populate is deferred (see
// bb_data.h). Uses a trivial fake format entry (registered/reset via the
// real bb_serialize_format API) plus the real bb_meminfo_heap_snap_desc
// fixture where a real descriptor adds value.

#include "unity.h"

#include "bb_data.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"

#include "../../components/bb_meminfo/bb_meminfo_heap_snap.h"

#include <stddef.h>
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

static bb_err_t dt_gather_ok(void *dst, void *ctx)
{
    ((dt_snap_t *)dst)->n = *(int64_t *)ctx;
    return BB_OK;
}

static bb_err_t dt_gather_fail(void *dst, void *ctx)
{
    (void)dst;
    (void)ctx;
    return BB_ERR_INVALID_STATE;
}

// Adapter: bb_meminfo_heap_snap_fill() takes a single out-param, not the
// (dst, ctx) shape bb_data_gather_fn requires -- wrap rather than cast the
// fn pointer across a mismatched signature.
static bb_err_t dt_gather_meminfo(void *dst, void *ctx)
{
    (void)ctx;
    return bb_meminfo_heap_snap_fill((bb_meminfo_heap_snap_t *)dst);
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
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_data_render(BB_FORMAT_JSON, "dt.override", scratch, sizeof(scratch), buf, sizeof(buf),
                                      &out_len));
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
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render(BB_FORMAT_JSON, "dt.meminfo", &scratch, sizeof(scratch), buf,
                                             sizeof(buf), &out_len));
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
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_data_render(BB_FORMAT_JSON, "dt.nope", scratch, sizeof(scratch), buf, sizeof(buf),
                                      &out_len));
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
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED,
                       bb_data_render(BB_FORMAT_JSON, "dt.nofmt", scratch, sizeof(scratch), buf, sizeof(buf),
                                      &out_len));
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
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                       bb_data_render(BB_FORMAT_JSON, "dt.gatherfail", scratch, sizeof(scratch), buf, sizeof(buf),
                                      &out_len));
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
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_data_render(BB_FORMAT_JSON, "dt.overflow", scratch, sizeof(scratch), buf, sizeof(buf),
                                      &out_len));
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
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_data_render(BB_FORMAT_JSON, "dt.scratch", scratch, sizeof(scratch), buf, sizeof(buf),
                                      &out_len));
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
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED,
                       bb_data_render(BB_FORMAT_JSON, "dt.noop", scratch, sizeof(scratch), buf, sizeof(buf),
                                      &out_len));
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

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_data_render(BB_FORMAT_JSON, NULL, scratch, sizeof(scratch), buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_data_render(BB_FORMAT_JSON, "dt.nullargs", NULL, sizeof(scratch), buf, sizeof(buf),
                                      &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_data_render(BB_FORMAT_JSON, "dt.nullargs", scratch, sizeof(scratch), NULL, sizeof(buf),
                                      &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_data_render(BB_FORMAT_JSON, "dt.nullargs", scratch, sizeof(scratch), buf, sizeof(buf),
                                      NULL));
}
