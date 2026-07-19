// Host tests for bb_serialize_json_stream_render() -- the flush-sink bridge
// (B1-1077 PR-1). Capture-to-string flush_fn, no httpd involved (the HTTP
// adapter, bb_http_serialize_stream(), is covered separately in
// test_bb_http_serialize_stream.c against the host bb_http_resp_send_chunk
// backend).

#include "unity.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixture 1: flat scalars -- small enough to render in a single flush.
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
// Fixture 2: array of strings, large enough to overflow the internal
// BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES (768) flush buffer and force
// multiple flush_fn calls.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// 1. Byte-identical guard: stream_render output == bb_serialize_json_render()
// output for the same fixed-scalar desc/snap.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_byte_identical_to_render(void)
{
    flat_snap_t snap = { .a = 42, .b = true };

    char   bounded_buf[128];
    size_t bounded_len = 0;
    bb_err_t bounded_rc = bb_serialize_json_render(&s_flat_desc, &snap,
                                                    bounded_buf, sizeof(bounded_buf), &bounded_len);
    TEST_ASSERT_EQUAL(BB_OK, bounded_rc);

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t stream_rc = bb_serialize_json_stream_render(&s_flat_desc, &snap,
                                                          capture_flush, &cap, NULL);
    TEST_ASSERT_EQUAL(BB_OK, stream_rc);
    TEST_ASSERT_EQUAL_UINT(bounded_len, cap.len);
    TEST_ASSERT_EQUAL_MEMORY(bounded_buf, cap.buf, bounded_len);
}

// ---------------------------------------------------------------------------
// 2. Multi-flush: a descriptor whose rendered output exceeds the internal
// flush buffer forces more than one flush_fn call.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_stream_multi_flush(void)
{
    const char *items[STREAM_ARR_COUNT];
    char        item_strs[STREAM_ARR_COUNT][16];
    for (int i = 0; i < STREAM_ARR_COUNT; i++) {
        snprintf(item_strs[i], sizeof(item_strs[i]), "item-%03d", i);
        items[i] = item_strs[i];
    }
    arr_snap_t snap = { .a = { .items = items, .count = STREAM_ARR_COUNT } };

    char   bounded_buf[CAPTURE_BUF_BYTES];
    size_t bounded_len = 0;
    bb_err_t bounded_rc = bb_serialize_json_render(&s_arr_desc, &snap,
                                                    bounded_buf, sizeof(bounded_buf), &bounded_len);
    TEST_ASSERT_EQUAL(BB_OK, bounded_rc);
    // Sanity: this fixture must actually exceed the flush buffer to prove
    // the multi-flush path -- if this ever shrinks below the flush buffer
    // size the test would silently degenerate to the single-flush case.
    TEST_ASSERT_GREATER_THAN(768, (int)bounded_len);

    capture_ctx_t cap = { .len = 0, .n_flushes = 0 };
    bb_err_t stream_rc = bb_serialize_json_stream_render(&s_arr_desc, &snap,
                                                          capture_flush, &cap, NULL);
    TEST_ASSERT_EQUAL(BB_OK, stream_rc);
    TEST_ASSERT_GREATER_THAN(1, (int)cap.n_flushes);
    TEST_ASSERT_EQUAL_UINT(bounded_len, cap.len);
    TEST_ASSERT_EQUAL_MEMORY(bounded_buf, cap.buf, bounded_len);
}

// ---------------------------------------------------------------------------
// 3. Abort: flush_fn sets *flush_failed on the first call -> stream_render
// returns non-BB_OK and no FURTHER flush_fn calls occur (sticky abort).
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

void test_bb_serialize_json_stream_abort_sticky_no_further_flush(void)
{
    const char *items[STREAM_ARR_COUNT];
    char        item_strs[STREAM_ARR_COUNT][16];
    for (int i = 0; i < STREAM_ARR_COUNT; i++) {
        snprintf(item_strs[i], sizeof(item_strs[i]), "item-%03d", i);
        items[i] = item_strs[i];
    }
    arr_snap_t snap = { .a = { .items = items, .count = STREAM_ARR_COUNT } };

    abort_ctx_t ac = { .cap = { .len = 0, .n_flushes = 0 }, .failed = false };
    bb_err_t rc = bb_serialize_json_stream_render(&s_arr_desc, &snap,
                                                   abort_flush, &ac, &ac.failed);

    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
    TEST_ASSERT_EQUAL_UINT(1, ac.cap.n_flushes);  // exactly one -- no further calls after abort
}

// ---------------------------------------------------------------------------
// flush_fn == NULL (no streaming) is exercised via every existing
// bb_serialize_json_render() test in test_bb_serialize_json.c -- the branch
// this PR adds (flush_fn set vs NULL) is covered by that whole suite plus
// the streaming tests above.
// ---------------------------------------------------------------------------
