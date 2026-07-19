// B1-1077 PR-2 -- BB_ARR_STREAM descriptor + the walker's new
// cardinality==BB_ARR_STREAM branch (bb_serialize_walk.c). Mirrors
// test_bb_serialize_nested_arr.c's fixture pattern (render via
// bb_serialize_json_render, golden strings built mechanically/independently
// of the writer). The FIXED (cardinality==0, the else branch) path is
// exercised by every other bb_serialize test file, UNMODIFIED by this PR --
// this file is additive-only coverage for the new branch's both arms.

#include "unity.h"
#include "bb_serialize.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixture: outer_t{ items[]: obj{ id:I64, label:STR } }, items STREAMED via
// bb_serialize_arr_stream_from_buf() over a caller-owned row_t[] buffer --
// the generic reusable iterator every real iter section (e.g. storage/nvs)
// wires the same way.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t id;
    char    label[16];
} row_t;

typedef struct {
    bb_serialize_arr_stream_t   items;
    bb_serialize_arr_buf_iter_t items_state;
} outer_t;

static const bb_serialize_field_t s_row_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(row_t, id) },
    { .key = "label", .type = BB_TYPE_STR, .offset = offsetof(row_t, label),
      .max_len = sizeof(((row_t *)0)->label) },
};

static const bb_serialize_field_t s_outer_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(outer_t, items),
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(row_t),
      .children = s_row_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_outer_desc = {
    .type_name = "outer_t",
    .fields = s_outer_fields,
    .n_fields = 1,
    .snap_size = sizeof(outer_t),
};

// ---------------------------------------------------------------------------
// Arm 1 (true): carrier.next set, depth < BB_SERIALIZE_MAX_DEPTH -- N=0 and
// N=40 (well past the old fixed-cap precedent of 16) prove no per-field cap.
// ---------------------------------------------------------------------------

void test_bb_serialize_arr_stream_render_zero_rows(void)
{
    row_t rows[1];  // unused -- N=0
    outer_t snap;
    snap.items = bb_serialize_arr_stream_from_buf(&snap.items_state, rows, 0, sizeof(row_t));

    char buf[64];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&s_outer_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"items\":[]}", buf);
}

void test_bb_serialize_arr_stream_render_forty_rows_no_truncation(void)
{
    row_t rows[40];
    for (int i = 0; i < 40; i++) {
        rows[i].id = i;
        snprintf(rows[i].label, sizeof(rows[i].label), "r%d", i);
    }
    outer_t snap;
    snap.items = bb_serialize_arr_stream_from_buf(&snap.items_state, rows, 40, sizeof(row_t));

    char buf[2048];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&s_outer_desc, &snap, buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Golden built mechanically, independent of the writer.
    char expected[2048];
    size_t elen = 0;
    elen += (size_t)snprintf(expected + elen, sizeof(expected) - elen, "{\"items\":[");
    for (int i = 0; i < 40; i++) {
        elen += (size_t)snprintf(expected + elen, sizeof(expected) - elen,
                                  "%s{\"id\":%d,\"label\":\"r%d\"}", i == 0 ? "" : ",", i, i);
    }
    snprintf(expected + elen, sizeof(expected) - elen, "]}");

    TEST_ASSERT_EQUAL_STRING(expected, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

// ---------------------------------------------------------------------------
// Arm 2 (false): carrier.next == NULL -- covers the `carrier.next && ...`
// short-circuit distinct from the depth-guard reason below. begin_arr/
// end_arr still fire (matches the FIXED path's own "no crash, empty array"
// convention for a NULL-items carrier).
// ---------------------------------------------------------------------------

void test_bb_serialize_arr_stream_null_next_emits_empty_array(void)
{
    outer_t snap;
    snap.items = (bb_serialize_arr_stream_t){ .next = NULL, .iter_ctx = NULL, .row_size = sizeof(row_t) };

    char buf[64];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&s_outer_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"items\":[]}", buf);
}

// ---------------------------------------------------------------------------
// Arm 2 (false), other reason: depth >= BB_SERIALIZE_MAX_DEPTH -- a
// self-referential STREAM chain (each level's one "kids" row is the next
// level) reaching past the cap. Mirrors test_bb_serialize_nested_arr.c's
// FIXED-array depth-boundary proof, for the STREAM branch.
// ---------------------------------------------------------------------------

typedef struct chain_node_s {
    int64_t                      marker;
    bb_serialize_arr_stream_t    kids;
    bb_serialize_arr_buf_iter_t  kids_state;
} chain_node_t;

static const bb_serialize_field_t s_chain_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    // Self-referential: each element's own "kids" field recurses through
    // the SAME table -- same idiom as test_bb_serialize.c's
    // s_deep_arr_fields (FIXED-array depth-guard fixture).
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t),
      .children = s_chain_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_chain_desc = {
    .type_name = "chain_node_t",
    .fields = s_chain_fields,
    .n_fields = 2,
    .snap_size = sizeof(chain_node_t),
};

_Static_assert(BB_SERIALIZE_MAX_DEPTH == 8,
               "s_chain_fields is self-referential -- any MAX_DEPTH change still terminates safely,"
               " but this test's expected-golden loop count below assumes 8");

void test_bb_serialize_arr_stream_depth_guard_bails_past_max_depth(void)
{
    // BB_SERIALIZE_MAX_DEPTH + 2 levels -- each level's one "kids" row is
    // the next level, walking BB_SERIALIZE_MAX_DEPTH + 1 real STREAM hops
    // before the guard bails.
    chain_node_t levels[BB_SERIALIZE_MAX_DEPTH + 2];
    for (unsigned i = 0; i < BB_SERIALIZE_MAX_DEPTH + 2; i++) {
        levels[i].marker = (int64_t)i;
        levels[i].kids = (bb_serialize_arr_stream_t){ .next = NULL, .iter_ctx = NULL, .row_size = sizeof(chain_node_t) };
    }
    for (unsigned i = 0; i < BB_SERIALIZE_MAX_DEPTH + 1; i++) {
        levels[i].kids = bb_serialize_arr_stream_from_buf(&levels[i].kids_state, &levels[i + 1], 1,
                                                            sizeof(chain_node_t));
    }

    char buf[1024];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&s_chain_desc, &levels[0], buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // At depth == BB_SERIALIZE_MAX_DEPTH the STREAM guard bails before
    // calling carrier.next() at all -- "kids":[] terminates the golden at
    // that level, exactly like the FIXED-array over-depth proof.
    char expected[1024];
    strcpy(expected, "{\"marker\":8,\"kids\":[]}");
    for (int i = BB_SERIALIZE_MAX_DEPTH - 1; i >= 0; i--) {
        char wrapped[sizeof(expected) + 64];
        snprintf(wrapped, sizeof(wrapped), "{\"marker\":%d,\"kids\":[%s]}", i, expected);
        strcpy(expected, wrapped);
    }
    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

// ---------------------------------------------------------------------------
// bb_serialize_arr_stream_from_buf() -- direct unit coverage of the pure
// generic iterator itself, independent of the walker.
// ---------------------------------------------------------------------------

void test_bb_serialize_arr_stream_from_buf_iterates_and_exhausts(void)
{
    row_t rows[2] = { { .id = 1, .label = "one" }, { .id = 2, .label = "two" } };
    bb_serialize_arr_buf_iter_t state;
    bb_serialize_arr_stream_t carrier = bb_serialize_arr_stream_from_buf(&state, rows, 2, sizeof(row_t));

    row_t out;
    TEST_ASSERT_TRUE(carrier.next(carrier.iter_ctx, &out));
    TEST_ASSERT_EQUAL_INT64(1, out.id);
    TEST_ASSERT_EQUAL_STRING("one", out.label);

    TEST_ASSERT_TRUE(carrier.next(carrier.iter_ctx, &out));
    TEST_ASSERT_EQUAL_INT64(2, out.id);
    TEST_ASSERT_EQUAL_STRING("two", out.label);

    TEST_ASSERT_FALSE(carrier.next(carrier.iter_ctx, &out));  // exhausted
    TEST_ASSERT_EQUAL(sizeof(row_t), carrier.row_size);
}

void test_bb_serialize_arr_stream_from_buf_null_buf_zero_count_is_safe(void)
{
    bb_serialize_arr_buf_iter_t state;
    bb_serialize_arr_stream_t carrier = bb_serialize_arr_stream_from_buf(&state, NULL, 0, sizeof(row_t));

    row_t out;
    // idx (0) >= count (0) short-circuits before ever touching the NULL
    // base pointer -- never a crash, immediately exhausted.
    TEST_ASSERT_FALSE(carrier.next(carrier.iter_ctx, &out));
}
