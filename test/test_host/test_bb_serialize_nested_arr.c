// B1-1056 -- PROVES (does not add engine code) that nested BB_TYPE_ARR-of-
// BB_TYPE_OBJ descriptors already round-trip through bb_serialize, up to
// BB_SERIALIZE_MAX_DEPTH. The walker (bb_serialize_walk.c) is one generic
// recursive fn -- a BB_TYPE_ARR field with elem_type==BB_TYPE_OBJ recurses
// through the SAME fn, so a child that is itself BB_TYPE_ARR nests one level
// deeper automatically, with no special-casing anywhere. This file is the
// evidence (architect finding, KB 1458) plus a regression guard, and is
// reusable by B1-785's parse work.

#include "unity.h"
#include "bb_serialize.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 1. TWO-LEVEL nested ARR-of-OBJ fixture:
//      outer{ groups[]: obj{ name:STR, items[]: obj{ id:I64, label:STR } } }
// -- exercises emit AND parse (render + populate) at two levels of ARR-of-OBJ
// nesting, with real counts (>=2 outer entries, >=2 inner entries each) and
// an embedded-quote/escaping case in a STR field, so the assertions are
// mutation-resistant (field-by-field comparison, not tautological).
// ---------------------------------------------------------------------------

typedef struct {
    int64_t id;
    char    label[16];
} item_t;

typedef struct {
    char               name[16];
    bb_serialize_arr_t items;  // item_t[]
} group_t;

typedef struct {
    bb_serialize_arr_t groups;  // group_t[]
} outer_t;

static const bb_serialize_field_t s_item_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(item_t, id) },
    { .key = "label", .type = BB_TYPE_STR, .offset = offsetof(item_t, label),
      .max_len = sizeof(((item_t *)0)->label) },
};

static const bb_serialize_field_t s_group_fields[] = {
    { .key = "name", .type = BB_TYPE_STR, .offset = offsetof(group_t, name),
      .max_len = sizeof(((group_t *)0)->name) },
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(group_t, items),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(item_t), .max_items = 4,
      .children = s_item_fields, .n_children = 2 },
};

static const bb_serialize_field_t s_outer_fields[] = {
    { .key = "groups", .type = BB_TYPE_ARR, .offset = offsetof(outer_t, groups),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(group_t), .max_items = 4,
      .children = s_group_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_outer_desc = {
    .type_name = "outer_t",
    .fields = s_outer_fields,
    .n_fields = 1,
    .snap_size = sizeof(outer_t),
};

// Byte-exact golden -- hand-written independently of the writer. group[1]'s
// first item label carries an embedded quote (`thr"ee`) to prove escaping
// survives round-trip through BOTH levels of nesting, not just the top one.
static const char *s_golden =
    "{\"groups\":["
      "{\"name\":\"alpha\",\"items\":["
        "{\"id\":1,\"label\":\"one\"},"
        "{\"id\":2,\"label\":\"two\"}"
      "]},"
      "{\"name\":\"beta\",\"items\":["
        "{\"id\":3,\"label\":\"thr\\\"ee\"},"
        "{\"id\":4,\"label\":\"four\"}"
      "]}"
    "]}";

void test_bb_serialize_nested_arr_render_matches_golden(void)
{
    item_t group0_items[2] = {
        { .id = 1, .label = "one" },
        { .id = 2, .label = "two" },
    };
    item_t group1_items[2] = {
        { .id = 3, .label = "thr\"ee" },
        { .id = 4, .label = "four" },
    };
    group_t groups[2] = {
        { .name = "alpha", .items = { .items = group0_items, .count = 2 } },
        { .name = "beta",  .items = { .items = group1_items, .count = 2 } },
    };
    outer_t snap = { .groups = { .items = groups, .count = 2 } };

    char buf[512];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&s_outer_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING(s_golden, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
    TEST_ASSERT_TRUE(out_len <= bb_serialize_json_bound(&s_outer_desc));
}

void test_bb_serialize_nested_arr_populate_roundtrip_matches_fixture(void)
{
    // Scan the SAME golden text (independent of the render test above -- if
    // one direction silently broke while the other didn't, this test alone
    // still fails).
    bb_serialize_json_tok_t pool[64];
    char arena[256];
    bb_serialize_json_tok_recorder_t rec;
    size_t golden_len = strlen(s_golden);

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_tok_recorder_init(
        &rec, s_golden, golden_len, pool, 64, arena, sizeof(arena)));
    bb_serialize_json_ingest_t sink = bb_serialize_json_tok_recorder_ingest(&rec);
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_bounded(s_golden, golden_len, &sink));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    // Pre-wire ALL possible destination storage (populate never allocates):
    // up to 4 groups, each with up to 4 items.
    item_t  item_storage[4][4];
    group_t group_storage[4];
    memset(item_storage, 0, sizeof(item_storage));
    memset(group_storage, 0, sizeof(group_storage));
    for (int i = 0; i < 4; i++) {
        group_storage[i].items = (bb_serialize_arr_t){ .items = item_storage[i], .count = 0 };
    }
    outer_t dst = { .groups = { .items = group_storage, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_outer_desc, &dst, &src);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Field-by-field comparison against the ORIGINAL fixture values (not a
    // re-render/string comparison) -- proves the scatter actually landed in
    // the right slots at both nesting levels.
    TEST_ASSERT_EQUAL_UINT(2, dst.groups.count);

    TEST_ASSERT_EQUAL_STRING("alpha", group_storage[0].name);
    TEST_ASSERT_EQUAL_UINT(2, group_storage[0].items.count);
    TEST_ASSERT_EQUAL_INT64(1, item_storage[0][0].id);
    TEST_ASSERT_EQUAL_STRING("one", item_storage[0][0].label);
    TEST_ASSERT_EQUAL_INT64(2, item_storage[0][1].id);
    TEST_ASSERT_EQUAL_STRING("two", item_storage[0][1].label);

    TEST_ASSERT_EQUAL_STRING("beta", group_storage[1].name);
    TEST_ASSERT_EQUAL_UINT(2, group_storage[1].items.count);
    TEST_ASSERT_EQUAL_INT64(3, item_storage[1][0].id);
    TEST_ASSERT_EQUAL_STRING("thr\"ee", item_storage[1][0].label);  // escaping survived
    TEST_ASSERT_EQUAL_INT64(4, item_storage[1][1].id);
    TEST_ASSERT_EQUAL_STRING("four", item_storage[1][1].label);

    // Untouched slots (group_storage[2], [3]) stay zeroed -- populate never
    // scribbles past what the source actually provided.
    TEST_ASSERT_EQUAL_STRING("", group_storage[2].name);
    TEST_ASSERT_EQUAL_UINT(0, group_storage[2].items.count);
}

// ---------------------------------------------------------------------------
// 2. Depth-guard proof, using the SAME nested-ARR-of-OBJ shape this ticket
// is about (not OBJ padding): a genuinely-FINITE (non-self-referential)
// chain of distinct field tables, each hop a real BB_TYPE_ARR field whose
// elem_type is BB_TYPE_OBJ, one hop per level. The chain is built once
// (s_lvlN_fields, N = 0..9) and reused at two different entry points --
// exactly the "reuse the same table at two starting depths" trick used by
// bb_serialize_populate.c's own depth-guard tests -- to get both the
// at-BB_SERIALIZE_MAX_DEPTH (must succeed) and one-hop-past-it (must fail
// closed) cases from one definition, with no risk of the two drifting apart.
//
// A self-referential descriptor (the cheaper way to build an "infinitely
// deep" table, used elsewhere for the RUNTIME walker's own guard, e.g.
// bb_serialize_json.c's deep-nesting render tests) can't be reused for the
// POSITIVE at-cap populate case here: bb_serialize_populate()'s pre-flight
// scan is purely descriptor-structural, so a self-referential table is
// ALWAYS rejected regardless of how shallow the real data actually is --
// only a genuinely finite table can prove populate succeeds AT the cap.
// ---------------------------------------------------------------------------

typedef struct chain_node_s {
    int64_t            marker;
    bb_serialize_arr_t kids;  // chain_node_t[], elem_type BB_TYPE_OBJ
} chain_node_t;

// r=0: terminal -- marker only, no further ARR hop.
static const bb_serialize_field_t s_lvl0_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
};
static const bb_serialize_field_t s_lvl1_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl0_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_lvl2_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl1_fields, .n_children = 2 },
};
static const bb_serialize_field_t s_lvl3_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl2_fields, .n_children = 2 },
};
static const bb_serialize_field_t s_lvl4_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl3_fields, .n_children = 2 },
};
static const bb_serialize_field_t s_lvl5_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl4_fields, .n_children = 2 },
};
static const bb_serialize_field_t s_lvl6_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl5_fields, .n_children = 2 },
};
static const bb_serialize_field_t s_lvl7_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl6_fields, .n_children = 2 },
};
static const bb_serialize_field_t s_lvl8_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl7_fields, .n_children = 2 },
};
static const bb_serialize_field_t s_lvl9_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(chain_node_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(chain_node_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(chain_node_t), .max_items = 1,
      .children = s_lvl8_fields, .n_children = 2 },
};

_Static_assert(BB_SERIALIZE_MAX_DEPTH == 8,
               "chain table above is hand-built for MAX_DEPTH==8 -- update s_lvlN_fields if this changes");

// Entry at s_lvl8_fields -- 8 real ARR-of-OBJ hops (root@depth0 -> ... ->
// innermost elements visited at depth 8, terminal, no field ever sits AT
// depth 8 structurally). Must succeed end-to-end.
static const bb_serialize_desc_t s_chain_at_max_desc = {
    .type_name = "chain_node_t",
    .fields = s_lvl8_fields,
    .n_fields = 2,
    .snap_size = sizeof(chain_node_t),
};

// Entry at s_lvl9_fields -- one hop deeper: the innermost surviving "kids"
// ARR field structurally sits AT depth 8 (BB_SERIALIZE_MAX_DEPTH). Must be
// rejected BB_ERR_NO_SPACE by populate's pre-flight scan.
static const bb_serialize_desc_t s_chain_over_max_desc = {
    .type_name = "chain_node_t",
    .fields = s_lvl9_fields,
    .n_fields = 2,
    .snap_size = sizeof(chain_node_t),
};

// Linear-cursor populate source driving straight off a `chain_node_t[]`
// array -- the chain is strictly linear (each node has at most one child),
// so one advancing index is sufficient state. `key` is checked for the
// scalar getter (only "marker" exists in this shape) but not for the
// container callbacks (only one container field, "kids", ever appears).
typedef struct {
    const chain_node_t *nodes;
    int                  cur;
} chain_src_ctx_t;

static bool chain_src_get_i64(void *vctx, const char *key, int64_t *out)
{
    chain_src_ctx_t *ctx = vctx;
    if (strcmp(key, "marker") != 0) return false;
    *out = ctx->nodes[ctx->cur].marker;
    return true;
}
static bool chain_src_get_u64(void *ctx, const char *key, uint64_t *out)
{
    (void)ctx; (void)key; (void)out;
    return false;
}
static bool chain_src_get_f64(void *ctx, const char *key, double *out)
{
    (void)ctx; (void)key; (void)out;
    return false;
}
static bool chain_src_get_bool(void *ctx, const char *key, bool *out)
{
    (void)ctx; (void)key; (void)out;
    return false;
}
static bool chain_src_get_str(void *ctx, const char *key, char *dst, size_t cap, size_t *out_len)
{
    (void)ctx; (void)key; (void)dst; (void)cap; (void)out_len;
    return false;
}
static bool chain_src_begin_obj(void *vctx, const char *key)
{
    (void)key;  // array element -- always NULL
    chain_src_ctx_t *ctx = vctx;
    ctx->cur++;
    return true;
}
static bool chain_src_end_obj(void *ctx) { (void)ctx; return true; }
static bool chain_src_begin_arr(void *ctx, const char *key, size_t *count)
{
    (void)ctx; (void)key;
    if (count) *count = 1;
    return true;
}
static bool chain_src_end_arr(void *ctx) { (void)ctx; return true; }

static const bb_serialize_populate_t s_chain_src = {
    .format_id = BB_FORMAT_NONE,
    .ctx = NULL,
    .get_i64 = chain_src_get_i64,
    .get_u64 = chain_src_get_u64,
    .get_f64 = chain_src_get_f64,
    .get_bool = chain_src_get_bool,
    .get_str = chain_src_get_str,
    .begin_obj = chain_src_begin_obj,
    .end_obj = chain_src_end_obj,
    .begin_arr = chain_src_begin_arr,
    .end_arr = chain_src_end_arr,
};

void test_bb_serialize_nested_arr_at_max_depth_render_populate_roundtrip(void)
{
    // Real, distinct chain_node_t instances, one per level -- nodes[0] is
    // the root, nodes[8] is the innermost (empty kids, terminating).
    chain_node_t nodes[BB_SERIALIZE_MAX_DEPTH + 1];
    for (int i = BB_SERIALIZE_MAX_DEPTH; i >= 0; i--) {
        nodes[i].marker = i;
        nodes[i].kids = (i == BB_SERIALIZE_MAX_DEPTH)
            ? (bb_serialize_arr_t){ .items = NULL, .count = 0 }
            : (bb_serialize_arr_t){ .items = &nodes[i + 1], .count = 1 };
    }

    char buf[1024];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&s_chain_at_max_desc, &nodes[0], buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Expected string built mechanically, independent of the writer. The
    // innermost level (r=0, s_lvl0_fields) is genuinely terminal -- it has
    // no "kids" field at all (unlike a self-referential table, which would
    // still emit an empty "kids":[] at the bottom) -- so its own rendering
    // omits the key entirely.
    char expected[1024];
    strcpy(expected, "{\"marker\":8}");
    for (int i = BB_SERIALIZE_MAX_DEPTH - 1; i >= 0; i--) {
        char wrapped[sizeof(expected) + 64];
        snprintf(wrapped, sizeof(wrapped), "{\"marker\":%d,\"kids\":[%s]}", i, expected);
        strcpy(expected, wrapped);
    }
    TEST_ASSERT_EQUAL_STRING(expected, buf);

    // Populate side: driven directly against a hand-rolled bb_serialize_
    // populate_t source over `nodes[]` (NOT via bb_serialize_json_scan_
    // bounded()/its tok recorder). This is deliberate, not a shortcut: the
    // tok recorder's own container-nesting stack is sized to
    // BB_SERIALIZE_MAX_DEPTH, matching the WALKER's semantic OBJ/ARR-hop
    // depth -- but the raw JSON TEXT for this two-container-per-level shape
    // (each level is an OBJ wrapped in an ARR) nests roughly twice as deep in
    // raw braces/brackets as the walker's semantic depth counter, so
    // scanning this exact golden text back in via bb_serialize_json_scan_
    // bounded() overflows the recorder's OWN stack (BB_ERR_NO_SPACE) before
    // populate() ever runs -- a separate, pre-existing constraint of the
    // JSON *scanning* layer's bounded-mode token stack, orthogonal to
    // bb_serialize_populate() core's field-table depth guard under test
    // here, and out of scope for this proving-only ticket (no engine code).
    // Driving bb_serialize_populate() directly isolates exactly what this
    // test is proving: the core walker + populate correctly handle a real,
    // non-self-referential ARR-of-OBJ chain at exactly BB_SERIALIZE_MAX_DEPTH.
    chain_src_ctx_t src_ctx = { .nodes = nodes, .cur = 0 };
    bb_serialize_populate_t src = s_chain_src;
    src.ctx = &src_ctx;

    chain_node_t dst_storage[BB_SERIALIZE_MAX_DEPTH];  // holds levels 1..8
    memset(dst_storage, 0, sizeof(dst_storage));
    for (int i = 0; i < BB_SERIALIZE_MAX_DEPTH - 1; i++) {
        dst_storage[i].kids = (bb_serialize_arr_t){ .items = &dst_storage[i + 1], .count = 0 };
    }
    chain_node_t dst = { .kids = { .items = dst_storage, .count = 0 } };

    rc = bb_serialize_populate(&s_chain_at_max_desc, &dst, &src);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    TEST_ASSERT_EQUAL_INT64(0, dst.marker);
    TEST_ASSERT_EQUAL_UINT(1, dst.kids.count);
    for (int i = 0; i < BB_SERIALIZE_MAX_DEPTH; i++) {
        TEST_ASSERT_EQUAL_INT64(i + 1, dst_storage[i].marker);
    }
    TEST_ASSERT_EQUAL_UINT(0, dst_storage[BB_SERIALIZE_MAX_DEPTH - 1].kids.count);  // innermost, empty
}

// Stub source whose callbacks TEST_FAIL if ever invoked -- proves populate's
// pre-flight scan rejects an over-depth descriptor BEFORE driving the
// source at all, not merely mid-scatter.
static bool never_get_i64(void *ctx, const char *key, int64_t *out)
{
    (void)ctx; (void)key; (void)out;
    TEST_FAIL_MESSAGE("get_i64 must not be called -- pre-flight scan should reject first");
    return false;
}
static bool never_get_u64(void *ctx, const char *key, uint64_t *out)
{
    (void)ctx; (void)key; (void)out;
    TEST_FAIL_MESSAGE("get_u64 must not be called -- pre-flight scan should reject first");
    return false;
}
static bool never_get_f64(void *ctx, const char *key, double *out)
{
    (void)ctx; (void)key; (void)out;
    TEST_FAIL_MESSAGE("get_f64 must not be called -- pre-flight scan should reject first");
    return false;
}
static bool never_get_bool(void *ctx, const char *key, bool *out)
{
    (void)ctx; (void)key; (void)out;
    TEST_FAIL_MESSAGE("get_bool must not be called -- pre-flight scan should reject first");
    return false;
}
static bool never_get_str(void *ctx, const char *key, char *dst, size_t cap, size_t *out_len)
{
    (void)ctx; (void)key; (void)dst; (void)cap; (void)out_len;
    TEST_FAIL_MESSAGE("get_str must not be called -- pre-flight scan should reject first");
    return false;
}
static bool never_begin_obj(void *ctx, const char *key)
{
    (void)ctx; (void)key;
    TEST_FAIL_MESSAGE("begin_obj must not be called -- pre-flight scan should reject first");
    return false;
}
static bool never_end_obj(void *ctx)
{
    (void)ctx;
    TEST_FAIL_MESSAGE("end_obj must not be called -- pre-flight scan should reject first");
    return false;
}
static bool never_begin_arr(void *ctx, const char *key, size_t *count)
{
    (void)ctx; (void)key; (void)count;
    TEST_FAIL_MESSAGE("begin_arr must not be called -- pre-flight scan should reject first");
    return false;
}
static bool never_end_arr(void *ctx)
{
    (void)ctx;
    TEST_FAIL_MESSAGE("end_arr must not be called -- pre-flight scan should reject first");
    return false;
}

static const bb_serialize_populate_t s_never_driven_src = {
    .format_id = BB_FORMAT_NONE,
    .ctx = NULL,
    .get_i64 = never_get_i64,
    .get_u64 = never_get_u64,
    .get_f64 = never_get_f64,
    .get_bool = never_get_bool,
    .get_str = never_get_str,
    .begin_obj = never_begin_obj,
    .end_obj = never_end_obj,
    .begin_arr = never_begin_arr,
    .end_arr = never_end_arr,
};

void test_bb_serialize_nested_arr_over_max_depth_populate_fails_closed(void)
{
    struct {
        chain_node_t dst;
        uint32_t     canary;
    } guard;
    memset(&guard, 0, sizeof(guard));
    guard.canary = 0xDEADBEEFu;
    guard.dst.marker = -1;  // sentinel -- must stay untouched

    bb_err_t rc = bb_serialize_populate(&s_chain_over_max_desc, &guard.dst, &s_never_driven_src);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_INT64(-1, guard.dst.marker);  // dst untouched -- rejected before any scatter
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, guard.canary);
}
