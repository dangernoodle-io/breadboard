// Host tests for bb_data's table (multi-row) producer shape (B1-1414):
// bb_data_bind()'s optional `rows` binding plus bb_data_render_rows(), the
// console-only per-row render entry point. Uses a real bb_serialize_console
// backend (registered/reset via the real bb_serialize_format API), exactly
// like test_bb_data.c's own dt_register_format() pattern for JSON.

#include "unity.h"

#include "bb_data.h"
#include "bb_serialize_console.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixture: a tiny 2-field row type + a controllable row-gather hook.
// ---------------------------------------------------------------------------

typedef struct {
    // 24 comfortably covers "r" + up to 19 digits (GCC's -Wformat-truncation
    // worst-case bound for %zu) + NUL, so the snprintf below can never
    // truncate regardless of the platform's size_t width.
    char    name[24];
    int64_t value;
} dtr_row_t;

static const bb_serialize_field_t s_dtr_row_fields[] = {
    { .key = "name", .type = BB_TYPE_STR, .offset = offsetof(dtr_row_t, name),
      .max_len = sizeof(((dtr_row_t *)0)->name) },
    { .key = "value", .type = BB_TYPE_I64, .offset = offsetof(dtr_row_t, value) },
};

static const bb_serialize_desc_t s_dtr_row_desc = {
    .type_name = "dtr_row_t",
    .fields    = s_dtr_row_fields,
    .n_fields  = 2,
    .snap_size = sizeof(dtr_row_t),
};

// Number of rows the fixture gather hook WOULD produce, up to `max_rows`.
// Set by each test before calling bb_data_render_rows().
static size_t s_dtr_gather_available = 0;
static size_t s_dtr_gather_calls     = 0;

static size_t dtr_row_gather(void *dst_rows, size_t max_rows, const bb_data_gather_args_t *args)
{
    (void)args;
    s_dtr_gather_calls++;

    dtr_row_t *rows = (dtr_row_t *)dst_rows;
    size_t     n     = s_dtr_gather_available;
    if (n > max_rows) n = max_rows;

    for (size_t i = 0; i < n; i++) {
        snprintf(rows[i].name, sizeof(rows[i].name), "r%zu", i);
        rows[i].value = (int64_t)(i * 10);
    }
    return n;
}

// A misbehaving row_gather that ignores `max_rows` entirely and reports
// `s_dtr_gather_available` rows regardless -- used to prove
// bb_data_render_rows()'s own defensive clamp, not this fixture's own
// max_rows-respecting behavior above.
static size_t dtr_row_gather_overreport(void *dst_rows, size_t max_rows, const bb_data_gather_args_t *args)
{
    (void)args;
    (void)max_rows;
    s_dtr_gather_calls++;

    dtr_row_t *rows = (dtr_row_t *)dst_rows;
    size_t     n     = s_dtr_gather_available;
    for (size_t i = 0; i < n; i++) {
        snprintf(rows[i].name, sizeof(rows[i].name), "r%zu", i);
        rows[i].value = (int64_t)(i * 10);
    }
    return n;
}

static const bb_data_row_binding_t s_dtr_row_binding = {
    .row_desc   = &s_dtr_row_desc,
    .row_gather = dtr_row_gather,
    .row_size   = sizeof(dtr_row_t),
};

// A scalar desc/gather pair -- used for bindings that deliberately carry NO
// table producer (binding->rows == NULL).
typedef struct {
    int64_t n;
} dtr_scalar_t;

static const bb_serialize_field_t s_dtr_scalar_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(dtr_scalar_t, n) },
};

static const bb_serialize_desc_t s_dtr_scalar_desc = {
    .type_name = "dtr_scalar_t",
    .fields    = s_dtr_scalar_fields,
    .n_fields  = 1,
    .snap_size = sizeof(dtr_scalar_t),
};

static bb_err_t dtr_scalar_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    ((dtr_scalar_t *)dst)->n = 1;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// emit_row spy -- captures every emitted row's line/len/row_idx in order.
// ---------------------------------------------------------------------------

#define DTR_CAPTURE_MAX 8
#define DTR_CAPTURE_LINE_MAX 96

typedef struct {
    char   line[DTR_CAPTURE_LINE_MAX];
    size_t len;
    size_t row_idx;
} dtr_captured_row_t;

static dtr_captured_row_t s_dtr_captured[DTR_CAPTURE_MAX];
static size_t             s_dtr_captured_count = 0;

static void dtr_emit_capture(const char *line, size_t len, size_t row_idx, void *ctx)
{
    (void)ctx;
    if (s_dtr_captured_count >= DTR_CAPTURE_MAX) return;

    dtr_captured_row_t *cap = &s_dtr_captured[s_dtr_captured_count++];
    strncpy(cap->line, line, sizeof(cap->line) - 1);
    cap->line[sizeof(cap->line) - 1] = '\0';
    cap->len                          = len;
    cap->row_idx                      = row_idx;
}

static void dtr_capture_reset(void)
{
    s_dtr_captured_count = 0;
    memset(s_dtr_captured, 0, sizeof(s_dtr_captured));
}

// ---------------------------------------------------------------------------
// Fixture setup helpers.
// ---------------------------------------------------------------------------

static void dtr_reset(void)
{
    bb_data_test_reset();
    bb_serialize_format_test_reset();
    dtr_capture_reset();
    s_dtr_gather_available = 0;
    s_dtr_gather_calls     = 0;
}

static void dtr_register_console_format(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_register_format());
}

// Row scratch big enough for every test's row count.
static dtr_row_t s_dtr_row_scratch[DTR_CAPTURE_MAX];

// ---------------------------------------------------------------------------
// bb_data_bind -- row-binding validation.
// ---------------------------------------------------------------------------

void test_bb_data_bind_row_size_mismatch_rejected(void)
{
    dtr_reset();

    bb_data_row_binding_t mismatched = {
        .row_desc   = &s_dtr_row_desc,
        .row_gather = dtr_row_gather,
        .row_size   = sizeof(dtr_row_t) - 1,  // deliberately wrong
    };
    bb_data_binding_t b = {
        .key = "dtr.badsize", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &mismatched,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

void test_bb_data_bind_rows_null_row_desc_rejected(void)
{
    dtr_reset();

    bb_data_row_binding_t no_desc = {
        .row_desc = NULL, .row_gather = dtr_row_gather, .row_size = sizeof(dtr_row_t),
    };
    bb_data_binding_t b = {
        .key = "dtr.nodesc", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &no_desc,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

void test_bb_data_bind_rows_null_row_gather_rejected(void)
{
    dtr_reset();

    bb_data_row_binding_t no_gather = {
        .row_desc = &s_dtr_row_desc, .row_gather = NULL, .row_size = sizeof(dtr_row_t),
    };
    bb_data_binding_t b = {
        .key = "dtr.nogather", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &no_gather,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

void test_bb_data_bind_rows_valid_accepted(void)
{
    dtr_reset();

    bb_data_binding_t b = {
        .key = "dtr.ok", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));
}

// ---------------------------------------------------------------------------
// bb_data_bind -- rows-only (dual-shape) binding validation (B1-1418).
// ---------------------------------------------------------------------------

// A pure table producer -- desc/gather both NULL, rows the only egress --
// must be legal now (previously required a throwaway scalar desc/gather
// pair).
void test_bb_data_bind_rows_only_no_scalar_accepted(void)
{
    dtr_reset();

    bb_data_binding_t b = {
        .key = "dtr.rowsonly", .desc = NULL, .gather = NULL, .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));
}

// `apply` durably writes through the binding's `desc` (bb_data_commit()'s
// `slot->desc->snap_size` deref) -- meaningless, and a NULL-deref waiting to
// happen, on a rows-only binding that has no `desc` at all. See
// bb_data_bind()'s own doc for why this must be rejected at bind time
// instead of surfacing as a NULL deref downstream in bb_data_commit().
static bb_err_t dtr_apply_noop(const void *snap, const bb_data_apply_args_t *args)
{
    (void)snap;
    (void)args;
    return BB_OK;
}

// Mutation-tested (firmware review LOW finding on this commit): deleting
// bb_data_bind()'s `!binding->desc && binding->apply` line makes this test
// FAIL (confirmed: bind then wrongly returns BB_OK). Inverting it to check
// `binding->gather` instead of `binding->desc` does NOT make this (or any
// other) test fail -- confirmed by an actual mutation run, not just read.
// That's not a gap in this test: bb_data_bind()'s own OUTER dual-shape gate
// (a few lines up -- `scalar_shape`/`rows_only_shape`) already guarantees
// `desc` and `gather` share the same nullness on every binding that reaches
// this line (scalar requires both non-NULL, rows-only requires both NULL,
// every other combination is rejected before this point) -- so checking
// `!binding->desc` vs `!binding->gather` here is behaviorally IDENTICAL for
// every input that can ever reach it. No test (this one or a new one) can
// discriminate that specific inversion without first breaking the outer
// gate's own invariant, which is a different check with its own dedicated
// regression tests above (test_bb_data_bind_desc_without_gather_rejected,
// test_bb_data_bind_gather_without_desc_rejected).
void test_bb_data_bind_rows_only_with_apply_rejected(void)
{
    dtr_reset();

    bb_data_binding_t b = {
        .key = "dtr.rowsonlyapply", .desc = NULL, .gather = NULL,
        .apply = dtr_apply_noop, .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

// Regression guard: desc without gather is still rejected (half-specified
// scalar shape, not the rows-only shape).
void test_bb_data_bind_desc_without_gather_rejected(void)
{
    dtr_reset();

    bb_data_binding_t b = {
        .key = "dtr.descnogather", .desc = &s_dtr_scalar_desc, .gather = NULL,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

// Regression guard: gather without desc is still rejected (half-specified
// scalar shape, not the rows-only shape).
void test_bb_data_bind_gather_without_desc_rejected(void)
{
    dtr_reset();

    bb_data_binding_t b = {
        .key = "dtr.gathernodesc", .desc = NULL, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

// All three of desc/gather/rows absent -- a binding with nothing to render
// at all.
void test_bb_data_bind_all_absent_rejected(void)
{
    dtr_reset();

    bb_data_binding_t b = { .key = "dtr.allabsent", .desc = NULL, .gather = NULL, .rows = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_bind(&b));
}

// ---------------------------------------------------------------------------
// bb_data_render -- guard against a rows-only binding's NULL desc/gather
// deref (B1-1418; see bb_data.c's own "NOT dead code" comment on this
// guard -- reachable via the SSE broadcaster's generic sweep, not just this
// direct call).
// ---------------------------------------------------------------------------

void test_bb_data_render_rows_only_binding_returns_unsupported(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = {
        .key = "dtr.renderrowsonly", .desc = NULL, .gather = NULL, .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    static char   scratch[64];
    static char   out_buf[64];
    size_t        out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_CONSOLE, .key = "dtr.renderrowsonly",
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = out_buf, .buf_cap = sizeof(out_buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_render(&req));
}

// ---------------------------------------------------------------------------
// bb_data_render_rows -- rows-only binding renders successfully (the whole
// point of this shape: a pure table producer with no scalar egress at all).
// ---------------------------------------------------------------------------

void test_bb_data_render_rows_rows_only_binding_renders_content(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = {
        .key = "dtr.rowsonlyrender", .desc = NULL, .gather = NULL, .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available = 2;

    bb_data_render_rows_req_t req = {
        .key = "dtr.rowsonlyrender", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));

    TEST_ASSERT_EQUAL_UINT(2, s_dtr_captured_count);
    TEST_ASSERT_EQUAL_STRING("name=r0 value=0", s_dtr_captured[0].line);
    TEST_ASSERT_EQUAL_STRING("name=r1 value=10", s_dtr_captured[1].line);
}

void test_bb_data_bind_rows_omitted_defaults_null(void)
{
    dtr_reset();

    // `rows` omitted from the struct literal -- must default to NULL, same
    // zero-init convention as replay_kind/apply.
    bb_data_binding_t b = { .key = "dtr.norows", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    dtr_register_console_format();
    bb_data_render_rows_req_t req = {
        .key = "dtr.norows", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_render_rows(&req));
}

// ---------------------------------------------------------------------------
// bb_data_render_rows -- THE non-negotiable content-assertion test.
// ---------------------------------------------------------------------------

void test_bb_data_render_rows_emits_content_per_row_in_order(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = {
        .key = "dtr.content", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available = 3;

    bb_data_render_rows_req_t req = {
        .key = "dtr.content", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));

    TEST_ASSERT_EQUAL(1, s_dtr_gather_calls);
    TEST_ASSERT_EQUAL_UINT(3, s_dtr_captured_count);

    TEST_ASSERT_EQUAL_UINT(0, s_dtr_captured[0].row_idx);
    TEST_ASSERT_EQUAL_STRING("name=r0 value=0", s_dtr_captured[0].line);
    TEST_ASSERT_EQUAL_UINT(strlen(s_dtr_captured[0].line), s_dtr_captured[0].len);

    TEST_ASSERT_EQUAL_UINT(1, s_dtr_captured[1].row_idx);
    TEST_ASSERT_EQUAL_STRING("name=r1 value=10", s_dtr_captured[1].line);

    TEST_ASSERT_EQUAL_UINT(2, s_dtr_captured[2].row_idx);
    TEST_ASSERT_EQUAL_STRING("name=r2 value=20", s_dtr_captured[2].line);
}

void test_bb_data_render_rows_zero_rows_emits_nothing(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = {
        .key = "dtr.zero", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available = 0;

    bb_data_render_rows_req_t req = {
        .key = "dtr.zero", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL(1, s_dtr_gather_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_dtr_captured_count);
}

void test_bb_data_render_rows_max_rows_below_available_truncates(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = {
        .key = "dtr.trunc", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available = 5;  // more than the req's max_rows below

    bb_data_render_rows_req_t req = {
        .key = "dtr.trunc", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = 2,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL_UINT(2, s_dtr_captured_count);
    TEST_ASSERT_EQUAL_STRING("name=r0 value=0", s_dtr_captured[0].line);
    TEST_ASSERT_EQUAL_STRING("name=r1 value=10", s_dtr_captured[1].line);
}

// ---------------------------------------------------------------------------
// bb_data_render_rows -- error/gating paths.
// ---------------------------------------------------------------------------

void test_bb_data_render_rows_binding_without_rows_returns_unsupported(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = { .key = "dtr.norows2", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    bb_data_render_rows_req_t req = {
        .key = "dtr.norows2", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL(0, s_dtr_gather_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_dtr_captured_count);
}

void test_bb_data_render_rows_unbound_key_returns_not_found(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_render_rows_req_t req = {
        .key = "dtr.nope", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_render_rows(&req));
}

void test_bb_data_render_rows_non_console_format_returns_unsupported(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = {
        .key = "dtr.jsonfmt", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available = 2;

    bb_data_render_rows_req_t req = {
        .key = "dtr.jsonfmt", .fmt = BB_FORMAT_JSON,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL(0, s_dtr_gather_calls);  // true no-op: row_gather never invoked
}

void test_bb_data_render_rows_console_format_not_registered_returns_unsupported(void)
{
    dtr_reset();
    // Deliberately no dtr_register_console_format() call -- BB_FORMAT_CONSOLE
    // has no registered renderer.

    bb_data_binding_t b = {
        .key = "dtr.noreg", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available = 2;

    bb_data_render_rows_req_t req = {
        .key = "dtr.noreg", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL(0, s_dtr_gather_calls);
}

void test_bb_data_render_rows_null_args_return_invalid_arg(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = {
        .key = "dtr.nullargs", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render_rows(NULL));

    bb_data_render_rows_req_t req_no_key = {
        .key = NULL, .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render_rows(&req_no_key));

    bb_data_render_rows_req_t req_no_scratch = {
        .key = "dtr.nullargs", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = NULL, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render_rows(&req_no_scratch));

    bb_data_render_rows_req_t req_no_emit = {
        .key = "dtr.nullargs", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = NULL,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_render_rows(&req_no_emit));
}

// A render carrying a non-NULL query is forwarded to the row-gather hook
// byte-for-byte via bb_data_gather_args_t.query, exactly like
// bb_data_render()'s own gather-forwarding contract.
static const char *s_dtr_query_expected_type = NULL;

static size_t dtr_row_gather_query(void *dst_rows, size_t max_rows, const bb_data_gather_args_t *args)
{
    TEST_ASSERT_EQUAL_STRING(s_dtr_query_expected_type, bb_serialize_query_get(args->query, "type"));
    return dtr_row_gather(dst_rows, max_rows, args);
}

void test_bb_data_render_rows_forwards_query_to_row_gather(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_row_binding_t query_rows = {
        .row_desc = &s_dtr_row_desc, .row_gather = dtr_row_gather_query, .row_size = sizeof(dtr_row_t),
    };
    bb_data_binding_t b = {
        .key = "dtr.query", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &query_rows,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available     = 1;
    s_dtr_query_expected_type  = "raw";

    bb_serialize_query_t query = {
        .params = { { .key = "type", .value = "raw" } },
        .count  = 1,
    };

    bb_data_render_rows_req_t req = {
        .key = "dtr.query", .fmt = BB_FORMAT_CONSOLE, .query = &query,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL_UINT(1, s_dtr_captured_count);
}

// A row_gather that ignores `max_rows` and reports MORE rows than
// requested -- bb_data_render_rows() clamps to `req->max_rows` rather than
// emitting past it (defensive against a misbehaving producer; the wider
// row_scratch buffer below still has room for the over-report, so this
// proves the CLAMP, not a buffer overrun).
void test_bb_data_render_rows_row_gather_overreport_clamps_to_max_rows(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_row_binding_t overreport_rows = {
        .row_desc = &s_dtr_row_desc, .row_gather = dtr_row_gather_overreport, .row_size = sizeof(dtr_row_t),
    };
    bb_data_binding_t b = {
        .key = "dtr.overreport", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &overreport_rows,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available = 5;  // more than the req's max_rows below

    bb_data_render_rows_req_t req = {
        .key = "dtr.overreport", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = 2,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL_UINT(2, s_dtr_captured_count);  // clamped, not the over-reported 5
}

// A fake CONSOLE format entry whose render fn always fails -- proves a
// per-row render error propagates immediately (stopping further rows)
// rather than being swallowed.
static bb_err_t dtr_render_fail(const bb_serialize_desc_t *desc, const void *snap,
                                 char *buf, size_t cap, size_t *out_len)
{
    (void)desc;
    (void)snap;
    (void)buf;
    (void)cap;
    if (out_len) *out_len = 0;
    return BB_ERR_INVALID_STATE;
}

static void dtr_register_failing_console_format(void)
{
    static const bb_serialize_format_entry_t entry = { .render = dtr_render_fail, .parse = NULL };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_CONSOLE, &entry));
}

// ---------------------------------------------------------------------------
// Row content that exceeds bb_data.c's internal BB_DATA_ROW_LINE_MAX_BYTES
// (160) -- pins the currently-documented truncation behaviour rather than
// leaving it untested (see bb_data.c's own comment on that #define).
// ---------------------------------------------------------------------------

typedef struct {
    char name[200];
} dtr_long_row_t;

static const bb_serialize_field_t s_dtr_long_row_fields[] = {
    { .key = "name", .type = BB_TYPE_STR, .offset = offsetof(dtr_long_row_t, name),
      .max_len = sizeof(((dtr_long_row_t *)0)->name) },
};

static const bb_serialize_desc_t s_dtr_long_row_desc = {
    .type_name = "dtr_long_row_t",
    .fields    = s_dtr_long_row_fields,
    .n_fields  = 1,
    .snap_size = sizeof(dtr_long_row_t),
};

// Fills one row whose rendered console line ("name=" + 199 'x's, ~204 bytes)
// exceeds bb_data.c's 160-byte internal line buffer.
static size_t dtr_long_row_gather(void *dst_rows, size_t max_rows, const bb_data_gather_args_t *args)
{
    (void)args;
    if (max_rows == 0) return 0;

    dtr_long_row_t *rows = (dtr_long_row_t *)dst_rows;
    memset(rows[0].name, 'x', sizeof(rows[0].name) - 1);
    rows[0].name[sizeof(rows[0].name) - 1] = '\0';
    return 1;
}

static const bb_data_row_binding_t s_dtr_long_row_binding = {
    .row_desc   = &s_dtr_long_row_desc,
    .row_gather = dtr_long_row_gather,
    .row_size   = sizeof(dtr_long_row_t),
};

static dtr_long_row_t s_dtr_long_row_scratch[1];

// A dedicated capture callback (rather than dtr_emit_capture/DTR_CAPTURE_
// LINE_MAX == 96) so the captured line itself is never truncated by the
// TEST fixture's own buffer -- only bb_data_render_rows()'s internal
// 160-byte line buffer is under test here.
#define DTR_LONG_CAPTURE_CAP 256
static char   s_dtr_long_line[DTR_LONG_CAPTURE_CAP];
static size_t s_dtr_long_len            = 0;
static size_t s_dtr_long_capture_count  = 0;

static void dtr_emit_capture_long(const char *line, size_t len, size_t row_idx, void *ctx)
{
    (void)row_idx;
    (void)ctx;
    s_dtr_long_capture_count++;

    size_t n = len < sizeof(s_dtr_long_line) - 1 ? len : sizeof(s_dtr_long_line) - 1;
    memcpy(s_dtr_long_line, line, n);
    s_dtr_long_line[n] = '\0';
    s_dtr_long_len     = len;
}

void test_bb_data_render_rows_row_exceeding_line_bound_truncates_silently(void)
{
    dtr_reset();
    dtr_register_console_format();

    bb_data_binding_t b = {
        .key = "dtr.longline", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_long_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_long_capture_count = 0;
    s_dtr_long_len           = 0;
    memset(s_dtr_long_line, 0, sizeof(s_dtr_long_line));

    bb_data_render_rows_req_t req = {
        .key = "dtr.longline", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_long_row_scratch, .max_rows = 1,
        .emit_row = dtr_emit_capture_long,
    };

    // Pins bb_data.c's documented behaviour for a row whose rendered console
    // line exceeds BB_DATA_ROW_LINE_MAX_BYTES (160): the underlying console
    // render fn truncates silently (still NUL-terminated, BB_OK), NOT an
    // error -- emit_row still fires exactly once, with `len` reflecting the
    // truncated length (159 == 160 - 1 for the NUL), not the row's full
    // ~204-byte rendered content.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL_UINT(1, s_dtr_long_capture_count);
    TEST_ASSERT_EQUAL_UINT(159, s_dtr_long_len);
    TEST_ASSERT_EQUAL_UINT(159, strlen(s_dtr_long_line));
    TEST_ASSERT_EQUAL_INT(0, strncmp(s_dtr_long_line, "name=", 5));
}

// ---------------------------------------------------------------------------
// bb_data_commit -- rebind-to-rows-only residual (firmware review MEDIUM
// finding on this commit): a key first bound scalar (desc+apply), parsed
// successfully (bb_data_parse() gates only on `apply`, never `desc`), then
// REBOUND to the desc-less rows-only shape before the matching
// bb_data_commit() call -- the already-parsed `bb_data_parsed_t` still holds
// the stale slot, now desc == NULL. Pins bb_data_commit()'s own `!slot->desc`
// guard (mirroring bb_data_render()'s) rather than a NULL-deref crash.
// ---------------------------------------------------------------------------

void test_bb_data_commit_stale_parse_after_rebind_rows_only_returns_unsupported(void)
{
    dtr_reset();

    bb_data_binding_t scalar_with_apply = {
        .key = "dtr.rebind", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .apply = dtr_apply_noop,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&scalar_with_apply));

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_register_format());

    static char parse_scratch[4096];
    const char *body = "{\"n\":5}";
    bb_data_parse_req_t parse_req = {
        .fmt = BB_FORMAT_JSON, .key = "dtr.rebind",
        .body = body, .body_len = strlen(body),
        .parse_scratch = parse_scratch, .parse_scratch_cap = sizeof(parse_scratch),
    };
    bb_data_parsed_t parsed;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_parse(&parse_req, &parsed));

    // Rebind the SAME key rows-only (desc/gather/apply all NULL) before
    // committing the stale `parsed` above.
    bb_data_binding_t rows_only = {
        .key = "dtr.rebind", .desc = NULL, .gather = NULL, .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&rows_only));

    static char dst_scratch[64];
    bb_data_commit_req_t commit_req = {
        .mode = BB_DATA_APPLY_POST, .dst_scratch = dst_scratch, .dst_scratch_cap = sizeof(dst_scratch),
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_data_commit(&parsed, &commit_req));
}

void test_bb_data_render_rows_row_render_failure_propagates(void)
{
    dtr_reset();
    dtr_register_failing_console_format();

    bb_data_binding_t b = {
        .key = "dtr.renderfail", .desc = &s_dtr_scalar_desc, .gather = dtr_scalar_gather,
        .rows = &s_dtr_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));

    s_dtr_gather_available = 2;

    bb_data_render_rows_req_t req = {
        .key = "dtr.renderfail", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_dtr_row_scratch, .max_rows = DTR_CAPTURE_MAX,
        .emit_row = dtr_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL_UINT(0, s_dtr_captured_count);  // fails on the first row, emit_row never reached
}
