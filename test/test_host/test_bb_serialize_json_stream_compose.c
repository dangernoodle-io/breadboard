// Host tests for bb_serialize_json_stream_compose_render() -- the composed-
// document counterpart to bb_serialize_json_stream_render() (B1-1097 PR-2).
// Capture-to-string flush_fn, no httpd involved (the HTTP adapter,
// bb_http_serialize_stream_compose(), is covered separately in
// test_bb_http_serialize_stream_compose.c).

#include "unity.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Two distinct single-field descs (distinct keys) -- lets the RAW-shape
// golden test merge two entries flat without producing a duplicate JSON key.
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

// Root-wire-descriptor fixture for the mixed-shape test: a top-level "ok"
// scalar plus a nested "network" object -- the shape bb_health's own root
// descriptor takes (flat scalars + a nested OBJ field), walked RAW so its
// fields merge directly into the document root.
typedef struct {
    bool up;
} net_t;

static const bb_serialize_field_t s_net_fields[] = {
    { .key = "up", .type = BB_TYPE_BOOL, .offset = offsetof(net_t, up) },
};

typedef struct {
    bool  ok;
    net_t network;
} root_snap_t;

static const bb_serialize_field_t s_root_fields[] = {
    { .key = "ok", .type = BB_TYPE_BOOL, .offset = offsetof(root_snap_t, ok) },
    { .key = "network", .type = BB_TYPE_OBJ, .offset = offsetof(root_snap_t, network),
      .children = s_net_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_root_desc = {
    .type_name = "root_snap_t",
    .fields = s_root_fields,
    .n_fields = 2,
    .snap_size = sizeof(root_snap_t),
};

// Array-of-strings fixture, large enough to overflow the internal
// BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES (768) flush buffer via a SINGLE
// compose entry -- forces multiple flush_fn calls.
#define STREAM_ARR_COUNT 100

typedef struct {
    bb_serialize_arr_t a;
} arr_snap_t;

static const bb_serialize_field_t s_arr_fields[] = {
    { .key = "a", .type = BB_TYPE_ARR, .offset = offsetof(arr_snap_t, a),
      .elem_type = BB_TYPE_STR, .max_len = 16, .max_items = STREAM_ARR_COUNT },
};

static const bb_serialize_desc_t s_arr_desc = {
    .type_name = "arr_snap_t",
    .fields = s_arr_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_snap_t),
};

// ---------------------------------------------------------------------------
// Capture-to-string flush sink
// ---------------------------------------------------------------------------

#define CAPTURE_BUF_BYTES 8192

typedef struct {
    char   buf[CAPTURE_BUF_BYTES];
    size_t len;
    size_t n_flushes;
} capture_ctx_t;

static void capture_flush(void *vctx, const char *data, size_t len)
{
    capture_ctx_t *cap = vctx;
    cap->n_flushes++;
    TEST_ASSERT_TRUE(cap->len + len <= sizeof(cap->buf));
    memcpy(cap->buf + cap->len, data, len);
    cap->len += len;
}

// Reference (bounded, non-streaming) render of the same groups[], same
// root-framing convention bb_serialize_json_stream_compose_render() itself
// uses (raw `{`/`}`, caller owns root framing -- see
// test_bb_serialize_compose_object_shape_json_golden() in
// test_bb_serialize_compose.c). Loops bb_serialize_compose_walk() once per
// group, same as the streaming wrapper under test.
static void bounded_compose_render(const bb_serialize_compose_group_t *groups, size_t n_groups,
                                    char *out, size_t out_cap)
{
    TEST_ASSERT_TRUE(out_cap >= 2);
    out[0] = '{';
    bb_serialize_json_ctx_t jctx;
    bb_serialize_json_ctx_init(&jctx, out + 1, out_cap - 2);
    bb_serialize_emit_t emit = bb_serialize_json_emit(&jctx);

    for (size_t i = 0; i < n_groups; i++) {
        const bb_serialize_compose_group_t *g = &groups[i];
        bb_err_t rc = bb_serialize_compose_walk(g->entries, g->n, g->shape, &emit);
        TEST_ASSERT_EQUAL(BB_OK, rc);
    }
    TEST_ASSERT_EQUAL(BB_OK, jctx.err);

    out[1 + jctx.len] = '}';
    out[1 + jctx.len + 1] = '\0';
}

// ---------------------------------------------------------------------------
// 1. Byte-identical guard, OBJECT shape ("sections"): each entry becomes a
// named nested object under the wrapper's raw root.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_compose_object_shape_byte_identical(void)
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

    char bounded_buf[128];
    bounded_compose_render(groups, 1, bounded_buf, sizeof(bounded_buf));
    TEST_ASSERT_EQUAL_STRING("{\"alpha\":{\"x\":10},\"beta\":{\"y\":true}}", bounded_buf);

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t stream_rc = bb_serialize_json_stream_compose_render(groups, 1, capture_flush, &cap, NULL);
    TEST_ASSERT_EQUAL(BB_OK, stream_rc);
    TEST_ASSERT_EQUAL_UINT(strlen(bounded_buf), cap.len);
    TEST_ASSERT_EQUAL_MEMORY(bounded_buf, cap.buf, cap.len);
}

// ---------------------------------------------------------------------------
// 2. Byte-identical guard, RAW shape ("root"): entries merge flatly into the
// wrapper's own raw root -- no per-entry wrapper attributable to compose.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_compose_raw_shape_byte_identical(void)
{
    x_snap_t xs = { .x = 10 };
    y_snap_t ys = { .y = true };
    const bb_serialize_compose_entry_t entries[] = {
        { .desc = &s_x_desc, .snap = &xs },
        { .desc = &s_y_desc, .snap = &ys },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = entries, .n = 2, .shape = BB_SERIALIZE_COMPOSE_RAW },
    };

    char bounded_buf[128];
    bounded_compose_render(groups, 1, bounded_buf, sizeof(bounded_buf));
    TEST_ASSERT_EQUAL_STRING("{\"x\":10,\"y\":true}", bounded_buf);

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t stream_rc = bb_serialize_json_stream_compose_render(groups, 1, capture_flush, &cap, NULL);
    TEST_ASSERT_EQUAL(BB_OK, stream_rc);
    TEST_ASSERT_EQUAL_UINT(strlen(bounded_buf), cap.len);
    TEST_ASSERT_EQUAL_MEMORY(bounded_buf, cap.buf, cap.len);
}

// ---------------------------------------------------------------------------
// 3. Mixed-shape document: a RAW group (root descriptor's flat scalar +
// nested-OBJ field, merging directly at the document root) followed by an
// OBJECT group (two named sections) -- the shape bb_serialize_compose_walk()
// cannot express in a single call of its own (one `shape` argument applied
// uniformly across its `entries[]`), but which this wrapper's per-group loop
// composes cleanly inside ONE pair of root braces. This is the case that
// motivated the groups[] rework (B1-1097 PR-2) -- it proves the wrapper can
// serve bb_health's actual /api/health document shape (PR-5).
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_compose_mixed_shape_raw_root_plus_object_sections(void)
{
    root_snap_t root = { .ok = true, .network = { .up = true } };
    const bb_serialize_compose_entry_t root_entries[] = {
        { .desc = &s_root_desc, .snap = &root },
    };

    x_snap_t xs = { .x = 10 };
    y_snap_t ys = { .y = true };
    const bb_serialize_compose_entry_t section_entries[] = {
        { .name = "alpha", .desc = &s_x_desc, .snap = &xs },
        { .name = "beta",  .desc = &s_y_desc, .snap = &ys },
    };

    const bb_serialize_compose_group_t groups[] = {
        { .entries = root_entries,    .n = 1, .shape = BB_SERIALIZE_COMPOSE_RAW },
        { .entries = section_entries, .n = 2, .shape = BB_SERIALIZE_COMPOSE_OBJECT },
    };

    char bounded_buf[256];
    bounded_compose_render(groups, 2, bounded_buf, sizeof(bounded_buf));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ok\":true,\"network\":{\"up\":true},\"alpha\":{\"x\":10},\"beta\":{\"y\":true}}", bounded_buf);

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t stream_rc = bb_serialize_json_stream_compose_render(groups, 2, capture_flush, &cap, NULL);
    TEST_ASSERT_EQUAL(BB_OK, stream_rc);
    TEST_ASSERT_EQUAL_UINT(strlen(bounded_buf), cap.len);
    TEST_ASSERT_EQUAL_MEMORY(bounded_buf, cap.buf, cap.len);
}

// ---------------------------------------------------------------------------
// 4. Multi-flush: a single composed entry whose rendered output exceeds the
// internal flush buffer forces more than one flush_fn call.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_compose_multi_flush(void)
{
    const char *items[STREAM_ARR_COUNT];
    char        item_strs[STREAM_ARR_COUNT][16];
    for (int i = 0; i < STREAM_ARR_COUNT; i++) {
        snprintf(item_strs[i], sizeof(item_strs[i]), "item-%03d", i);
        items[i] = item_strs[i];
    }
    arr_snap_t snap = { .a = { .items = items, .count = STREAM_ARR_COUNT } };
    const bb_serialize_compose_entry_t entries[] = {
        { .desc = &s_arr_desc, .snap = &snap },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = entries, .n = 1, .shape = BB_SERIALIZE_COMPOSE_RAW },
    };

    char bounded_buf[CAPTURE_BUF_BYTES];
    bounded_compose_render(groups, 1, bounded_buf, sizeof(bounded_buf));
    // Sanity: this fixture must actually exceed the flush buffer to prove
    // the multi-flush path -- if this ever shrinks below the flush buffer
    // size the test would silently degenerate to the single-flush case.
    TEST_ASSERT_GREATER_THAN(768, (int)strlen(bounded_buf));

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t stream_rc = bb_serialize_json_stream_compose_render(groups, 1, capture_flush, &cap, NULL);
    TEST_ASSERT_EQUAL(BB_OK, stream_rc);
    TEST_ASSERT_GREATER_THAN(1, (int)cap.n_flushes);
    TEST_ASSERT_EQUAL_UINT(strlen(bounded_buf), cap.len);
    TEST_ASSERT_EQUAL_MEMORY(bounded_buf, cap.buf, cap.len);
}

// ---------------------------------------------------------------------------
// 5. Abort: flush_fn sets *flush_failed on the first call -> stream returns
// non-BB_OK and no FURTHER flush_fn calls occur (sticky abort) -- the only
// remaining structural failure mode by design.
// ---------------------------------------------------------------------------

typedef struct {
    capture_ctx_t cap;
    volatile bool failed;
} abort_ctx_t;

static void abort_flush(void *vctx, const char *data, size_t len)
{
    abort_ctx_t *ac = vctx;
    capture_flush(&ac->cap, data, len);
    ac->failed = true;  // signal abort on the very first flush
}

void test_bb_serialize_json_stream_compose_abort_sticky_no_further_flush(void)
{
    const char *items[STREAM_ARR_COUNT];
    char        item_strs[STREAM_ARR_COUNT][16];
    for (int i = 0; i < STREAM_ARR_COUNT; i++) {
        snprintf(item_strs[i], sizeof(item_strs[i]), "item-%03d", i);
        items[i] = item_strs[i];
    }
    arr_snap_t snap = { .a = { .items = items, .count = STREAM_ARR_COUNT } };
    const bb_serialize_compose_entry_t entries[] = {
        { .desc = &s_arr_desc, .snap = &snap },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = entries, .n = 1, .shape = BB_SERIALIZE_COMPOSE_RAW },
    };

    abort_ctx_t ac = { .cap = { .len = 0, .n_flushes = 0 }, .failed = false };
    bb_err_t rc = bb_serialize_json_stream_compose_render(groups, 1, abort_flush, &ac, &ac.failed);

    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
    TEST_ASSERT_EQUAL_UINT(1, ac.cap.n_flushes);  // exactly one -- no further calls after abort
}

// ---------------------------------------------------------------------------
// 6. Gather failure aborts the whole compose walk (mid-group) -- the
// compose_err code (BB_ERR_VALIDATION, from the failing gather) takes
// precedence over ctx.err (which stays BB_OK -- the JSON written so far is
// well-formed), and whatever was emitted before the abort is still flushed
// (the streaming "partial JSON may already be flushed" tradeoff).
// ---------------------------------------------------------------------------

static bb_err_t gather_fails(void *snap, void *ctx)
{
    (void)snap;
    (void)ctx;
    return BB_ERR_VALIDATION;
}

void test_bb_serialize_json_stream_compose_gather_failure_propagates_and_flushes_partial(void)
{
    x_snap_t xs = { .x = 10 };
    y_snap_t ys = { .y = true };
    const bb_serialize_compose_entry_t entries[] = {
        { .desc = &s_x_desc, .snap = &xs },
        { .desc = &s_y_desc, .snap = &ys, .gather = gather_fails },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = entries, .n = 2, .shape = BB_SERIALIZE_COMPOSE_RAW },
    };

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t rc = bb_serialize_json_stream_compose_render(groups, 1, capture_flush, &cap, NULL);

    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_EQUAL_UINT(1, cap.n_flushes);
    TEST_ASSERT_EQUAL_UINT(strlen("{\"x\":10}"), cap.len);
    TEST_ASSERT_EQUAL_MEMORY("{\"x\":10}", cap.buf, cap.len);
}

// ---------------------------------------------------------------------------
// 7. A later group's gather failure aborts before that group's own entries
// are walked -- the prior group's fully-walked output is preserved/flushed
// (cross-group abort, distinct from the within-group case in 6 above).
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_compose_second_group_gather_failure_preserves_first_group(void)
{
    x_snap_t xs = { .x = 10 };
    const bb_serialize_compose_entry_t first_entries[] = {
        { .name = "alpha", .desc = &s_x_desc, .snap = &xs },
    };

    y_snap_t ys = { .y = true };
    const bb_serialize_compose_entry_t second_entries[] = {
        { .name = "beta", .desc = &s_y_desc, .snap = &ys, .gather = gather_fails },
    };

    const bb_serialize_compose_group_t groups[] = {
        { .entries = first_entries,  .n = 1, .shape = BB_SERIALIZE_COMPOSE_OBJECT },
        { .entries = second_entries, .n = 1, .shape = BB_SERIALIZE_COMPOSE_OBJECT },
    };

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t rc = bb_serialize_json_stream_compose_render(groups, 2, capture_flush, &cap, NULL);

    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_EQUAL_UINT(strlen("{\"alpha\":{\"x\":10}}"), cap.len);
    TEST_ASSERT_EQUAL_MEMORY("{\"alpha\":{\"x\":10}}", cap.buf, cap.len);
}

// ---------------------------------------------------------------------------
// 8. n_groups == 0 -> BB_OK, empty root object.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_compose_zero_groups_returns_empty_object(void)
{
    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t rc = bb_serialize_json_stream_compose_render(NULL, 0, capture_flush, &cap, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(strlen("{}"), cap.len);
    TEST_ASSERT_EQUAL_MEMORY("{}", cap.buf, cap.len);
}

// ---------------------------------------------------------------------------
// 9. A group with n == 0 among non-empty groups is a valid no-op for that
// group -- the surrounding groups' output is unaffected.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_compose_empty_group_among_nonempty_groups_is_noop(void)
{
    x_snap_t xs = { .x = 10 };
    const bb_serialize_compose_entry_t entries[] = {
        { .name = "alpha", .desc = &s_x_desc, .snap = &xs },
    };
    const bb_serialize_compose_group_t groups[] = {
        { .entries = NULL,    .n = 0, .shape = BB_SERIALIZE_COMPOSE_RAW },
        { .entries = entries, .n = 1, .shape = BB_SERIALIZE_COMPOSE_OBJECT },
        { .entries = NULL,    .n = 0, .shape = BB_SERIALIZE_COMPOSE_OBJECT },
    };

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t rc = bb_serialize_json_stream_compose_render(groups, 3, capture_flush, &cap, NULL);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(strlen("{\"alpha\":{\"x\":10}}"), cap.len);
    TEST_ASSERT_EQUAL_MEMORY("{\"alpha\":{\"x\":10}}", cap.buf, cap.len);
}
