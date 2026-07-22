// Host tests for bb_serialize_json_populate_from_tok() -- the adapter that
// binds bb_serialize_populate()'s pull-based source vtable to an
// already-scanned bb_serialize_json_tok_recorder_t (B1-896).

#include "unity.h"
#include "bb_serialize.h"
#include "bb_serialize_json.h"

#include <string.h>

#define POOL_CAP 64
#define ARENA_CAP 256

static bb_serialize_json_tok_t s_pool[POOL_CAP];
static char                    s_arena[ARENA_CAP];

static bb_err_t scan_into(bb_serialize_json_tok_recorder_t *rec, const char *doc, size_t len,
                           bb_serialize_json_tok_t *pool, size_t pool_cap, char *arena, size_t arena_cap)
{
    bb_err_t rc = bb_serialize_json_tok_recorder_init(rec, doc, len, pool, pool_cap, arena, arena_cap);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    bb_serialize_json_ingest_t sink = bb_serialize_json_tok_recorder_ingest(rec);
    return bb_serialize_json_scan_bounded(doc, len, &sink);
}

static bb_err_t scan_default(bb_serialize_json_tok_recorder_t *rec, const char *doc)
{
    return scan_into(rec, doc, strlen(doc), s_pool, POOL_CAP, s_arena, ARENA_CAP);
}

// ---------------------------------------------------------------------------
// 1. Flat scalars -- scan a literal JSON object, bind, drive
// bb_serialize_populate() against a two-string-field descriptor.
// ---------------------------------------------------------------------------

typedef struct {
    char name[16];
    char status[16];
} flat_snap_t;

static const bb_serialize_field_t s_flat_fields[] = {
    { .key = "name", .type = BB_TYPE_STR, .offset = offsetof(flat_snap_t, name), .max_len = sizeof(((flat_snap_t *)0)->name) },
    { .key = "status", .type = BB_TYPE_STR, .offset = offsetof(flat_snap_t, status), .max_len = sizeof(((flat_snap_t *)0)->status) },
};

static const bb_serialize_desc_t s_flat_desc = {
    .type_name = "flat_snap_t",
    .fields = s_flat_fields,
    .n_fields = 2,
    .snap_size = sizeof(flat_snap_t),
};

void test_bb_serialize_json_populate_flat_scalars(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"name\":\"unit-1\",\"status\":\"ok\"}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    flat_snap_t dst = { 0 };
    bb_err_t rc = bb_serialize_populate(&s_flat_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("unit-1", dst.name);
    TEST_ASSERT_EQUAL_STRING("ok", dst.status);
}

// ---------------------------------------------------------------------------
// 2. RE-DRIVABILITY -- one scanned doc, TWO independent populate cycles
// against TWO different descriptors (and two independent ctx cursors),
// each reading only its own field -- no interference.
// ---------------------------------------------------------------------------

typedef struct {
    char a[8];
} only_a_snap_t;

typedef struct {
    char b[8];
} only_b_snap_t;

static const bb_serialize_field_t s_a_fields[] = {
    { .key = "a", .type = BB_TYPE_STR, .offset = offsetof(only_a_snap_t, a), .max_len = sizeof(((only_a_snap_t *)0)->a) },
};
static const bb_serialize_desc_t s_a_desc = {
    .type_name = "only_a_snap_t", .fields = s_a_fields, .n_fields = 1, .snap_size = sizeof(only_a_snap_t),
};

static const bb_serialize_field_t s_b_fields[] = {
    { .key = "b", .type = BB_TYPE_STR, .offset = offsetof(only_b_snap_t, b), .max_len = sizeof(((only_b_snap_t *)0)->b) },
};
static const bb_serialize_desc_t s_b_desc = {
    .type_name = "only_b_snap_t", .fields = s_b_fields, .n_fields = 1, .snap_size = sizeof(only_b_snap_t),
};

void test_bb_serialize_json_populate_redrivable_two_descriptors(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"a\":\"one\",\"b\":\"two\"}"));

    bb_serialize_json_populate_ctx_t ctx_a;
    bb_serialize_populate_t src_a = bb_serialize_json_populate_from_tok(&ctx_a, &rec);
    bb_serialize_json_populate_ctx_t ctx_b;
    bb_serialize_populate_t src_b = bb_serialize_json_populate_from_tok(&ctx_b, &rec);

    only_a_snap_t dst_a = { 0 };
    only_b_snap_t dst_b = { 0 };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_a_desc, &dst_a, &src_a));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_b_desc, &dst_b, &src_b));

    TEST_ASSERT_EQUAL_STRING("one", dst_a.a);
    TEST_ASSERT_EQUAL_STRING("two", dst_b.b);
}

// ---------------------------------------------------------------------------
// 3. Absent field -- a descriptor field not present in the JSON leaves dst
// untouched (zero-initialized default preserved).
// ---------------------------------------------------------------------------

void test_bb_serialize_json_populate_absent_field_leaves_untouched(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"name\":\"unit-1\"}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    flat_snap_t dst;
    memset(&dst, 0, sizeof(dst));
    strncpy(dst.status, "keepme", sizeof(dst.status) - 1);

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_flat_desc, &dst, &src));
    TEST_ASSERT_EQUAL_STRING("unit-1", dst.name);
    TEST_ASSERT_EQUAL_STRING("keepme", dst.status);
}

// ---------------------------------------------------------------------------
// 4. Empty object -- every field is absent; populate succeeds, dst
// untouched.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_populate_empty_obj(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    flat_snap_t dst;
    memset(&dst, 0, sizeof(dst));
    strncpy(dst.name, "default", sizeof(dst.name) - 1);

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_flat_desc, &dst, &src));
    TEST_ASSERT_EQUAL_STRING("default", dst.name);
    TEST_ASSERT_EQUAL_STRING("", dst.status);
}

// ---------------------------------------------------------------------------
// 5. Nested object + extra/ignored keys, plus scalar (i64/u64/f64/bool)
// fields -- exercises begin_obj/end_obj, get_u64's success path, and a
// string value read via the recorder's ptr+len accessor at a nested scope.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t  id;
    uint64_t seq;
    double   temp;
    bool     armed;
} pos_snap_t;

typedef struct {
    char       label[16];
    pos_snap_t pos;
} nested_snap_t;

static const bb_serialize_field_t s_pos_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(pos_snap_t, id) },
    { .key = "seq", .type = BB_TYPE_U64, .offset = offsetof(pos_snap_t, seq) },
    { .key = "temp", .type = BB_TYPE_F64, .offset = offsetof(pos_snap_t, temp) },
    { .key = "armed", .type = BB_TYPE_BOOL, .offset = offsetof(pos_snap_t, armed) },
};

static const bb_serialize_field_t s_nested_fields[] = {
    { .key = "label", .type = BB_TYPE_STR, .offset = offsetof(nested_snap_t, label), .max_len = sizeof(((nested_snap_t *)0)->label) },
    { .key = "pos", .type = BB_TYPE_OBJ, .offset = offsetof(nested_snap_t, pos),
      .children = s_pos_fields, .n_children = 4 },
};

static const bb_serialize_desc_t s_nested_desc = {
    .type_name = "nested_snap_t", .fields = s_nested_fields, .n_fields = 2, .snap_size = sizeof(nested_snap_t),
};

void test_bb_serialize_json_populate_nested_obj_and_ignored_extra_keys(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec,
        "{\"ignored_top\":1,\"label\":\"unit-1\","
        "\"pos\":{\"ignored_nested\":true,\"id\":7,\"seq\":42,\"temp\":98.6,\"armed\":true}}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    nested_snap_t dst = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_nested_desc, &dst, &src));

    TEST_ASSERT_EQUAL_STRING("unit-1", dst.label);
    TEST_ASSERT_EQUAL_INT64(7, dst.pos.id);
    TEST_ASSERT_EQUAL_UINT64(42, dst.pos.seq);
    TEST_ASSERT_EQUAL_DOUBLE(98.6, dst.pos.temp);
    TEST_ASSERT_TRUE(dst.pos.armed);
}

// ---------------------------------------------------------------------------
// 6. Nested object absent -- present-but-wrong-type ("pos" is a string, not
// an object) exercises begin_obj's tok_is_obj false branch; nested fields
// stay untouched.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_populate_nested_obj_wrong_type_leaves_untouched(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"label\":\"unit-1\",\"pos\":\"not-an-object\"}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    nested_snap_t dst = { .pos = { .id = -1, .seq = 99, .temp = -1.0, .armed = true } };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_nested_desc, &dst, &src));

    TEST_ASSERT_EQUAL_STRING("unit-1", dst.label);
    TEST_ASSERT_EQUAL_INT64(-1, dst.pos.id);
    TEST_ASSERT_EQUAL_UINT64(99, dst.pos.seq);
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, dst.pos.temp);
    TEST_ASSERT_TRUE(dst.pos.armed);
}

// ---------------------------------------------------------------------------
// 6b. A present nested object with only SOME scalar fields present --
// exercises get_u64's absent (get_i64-fails) branch specifically, alongside
// get_i64's present branch in the same object scope.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_populate_nested_obj_partial_scalar_fields_absent(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"label\":\"unit-1\",\"pos\":{\"id\":5}}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    nested_snap_t dst = { .pos = { .id = -1, .seq = 99, .temp = -1.0, .armed = true } };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_nested_desc, &dst, &src));

    TEST_ASSERT_EQUAL_INT64(5, dst.pos.id);
    TEST_ASSERT_EQUAL_UINT64(99, dst.pos.seq);   // absent -- get_u64 false branch
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, dst.pos.temp); // absent
    TEST_ASSERT_TRUE(dst.pos.armed);              // absent
}

// ---------------------------------------------------------------------------
// 7. Array of OBJ -- exercises begin_obj's key==NULL (array-element) path
// and the per-element next_elem cursor advance.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t x;
    int64_t y;
} point_t;

typedef struct {
    bb_serialize_arr_t points;  // items: point_t[], elem_type BB_TYPE_OBJ
} arr_obj_snap_t;

static const bb_serialize_field_t s_point_fields[] = {
    { .key = "x", .type = BB_TYPE_I64, .offset = offsetof(point_t, x) },
    { .key = "y", .type = BB_TYPE_I64, .offset = offsetof(point_t, y) },
};

static const bb_serialize_field_t s_arr_obj_fields[] = {
    { .key = "points", .type = BB_TYPE_ARR, .offset = offsetof(arr_obj_snap_t, points),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(point_t), .max_items = 4,
      .children = s_point_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_arr_obj_desc = {
    .type_name = "arr_obj_snap_t", .fields = s_arr_obj_fields, .n_fields = 1, .snap_size = sizeof(arr_obj_snap_t),
};

void test_bb_serialize_json_populate_arr_of_obj(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec,
        "{\"points\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    point_t storage[4] = { 0 };
    arr_obj_snap_t dst = { .points = { .items = storage, .count = 0 } };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_arr_obj_desc, &dst, &src));

    TEST_ASSERT_EQUAL_UINT(2, dst.points.count);
    TEST_ASSERT_EQUAL_INT64(1, storage[0].x);
    TEST_ASSERT_EQUAL_INT64(2, storage[0].y);
    TEST_ASSERT_EQUAL_INT64(3, storage[1].x);
    TEST_ASSERT_EQUAL_INT64(4, storage[1].y);
}

// ---------------------------------------------------------------------------
// 8. Array of STR + array absent -- exercises get_str's key==NULL (array
// element) path plus begin_arr's tok_is_arr false branch (field missing
// entirely).
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_arr_t tags;  // items: char*[], elem_type BB_TYPE_STR
} arr_str_snap_t;

static const bb_serialize_field_t s_arr_str_fields[] = {
    { .key = "tags", .type = BB_TYPE_ARR, .offset = offsetof(arr_str_snap_t, tags),
      .elem_type = BB_TYPE_STR, .max_len = 8, .max_items = 3 },
};

static const bb_serialize_desc_t s_arr_str_desc = {
    .type_name = "arr_str_snap_t", .fields = s_arr_str_fields, .n_fields = 1, .snap_size = sizeof(arr_str_snap_t),
};

void test_bb_serialize_json_populate_arr_of_str(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"tags\":[\"one\",\"two\"]}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    char  buf0[8] = { 0 };
    char  buf1[8] = { 0 };
    char  buf2[8] = { 0 };
    char *ptrs[3] = { buf0, buf1, buf2 };
    arr_str_snap_t dst = { .tags = { .items = ptrs, .count = 0 } };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_arr_str_desc, &dst, &src));

    TEST_ASSERT_EQUAL_UINT(2, dst.tags.count);
    TEST_ASSERT_EQUAL_STRING("one", buf0);
    TEST_ASSERT_EQUAL_STRING("two", buf1);
}

void test_bb_serialize_json_populate_arr_absent_leaves_untouched(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    char  buf0[8] = { 0 };
    char *ptrs[3] = { buf0, NULL, NULL };
    arr_str_snap_t dst = { .tags = { .items = ptrs, .count = 99 } };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_arr_str_desc, &dst, &src));
    TEST_ASSERT_EQUAL_UINT(99, dst.tags.count);  // untouched -- absent field
}

// ---------------------------------------------------------------------------
// 9. String value that needs the recorder's escape-assembled (arena) ptr+len
// accessor, not just a zero-copy slice of the scanned buffer.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_populate_arena_backed_str_value(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"name\":\"ab\\ncd\",\"status\":\"ok\"}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    flat_snap_t dst = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_flat_desc, &dst, &src));

    TEST_ASSERT_EQUAL_MEMORY("ab\ncd", dst.name, 5);
    TEST_ASSERT_EQUAL_STRING("ok", dst.status);
}

// ---------------------------------------------------------------------------
// 10. Depth boundary (review follow-up, B1-896) -- the adapter's OWN
// fail-closed guard in populate_begin_obj/populate_begin_arr, exercised
// independently of whatever bb_serialize_populate() core does. A
// hand-crafted bb_serialize_json_tok_recorder_t (built directly, NOT via
// bb_serialize_json_scan_bounded()) chains 8 nested OBJ tokens (root +
// s_chain_pool[1..8], the legitimate BB_SERIALIZE_MAX_DEPTH-deep case --
// see populate_begin_obj's own comment) with one more ARR token nested
// under the deepest OBJ. A self-referential descriptor (mirroring
// bb_serialize_populate.c's own s_deep_desc/s_deep_arr_desc pattern) drives
// "arr" BEFORE "n" at every level so the ARR guard fires (st->depth ==
// BB_SERIALIZE_MAX_DEPTH already) before core's own pre-existing OBJ depth
// check trips on "n" one field later -- proving the adapter is self-safe
// even independent of that pre-existing core check. A canary placed
// immediately after the populate ctx's opaque _state[] buffer proves
// neither guard ever writes past the frame stack.
// ---------------------------------------------------------------------------

static bb_serialize_json_tok_t s_chain_pool[10];

// Builds root(0) -> 1 -> 2 -> ... -> 8, each linked by key "n" (8 nested OBJ
// tokens beyond the root -- the legitimate full-depth case), plus one ARR
// token "arr"(9) nested under token 8, the deepest OBJ.
static void build_depth_chain_rec(bb_serialize_json_tok_recorder_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    memset(s_chain_pool, 0, sizeof(s_chain_pool));
    rec->buf = "";
    rec->buf_len = 0;
    rec->pool = s_chain_pool;
    rec->pool_cap = 10;
    rec->pool_n = 10;

    for (int i = 0; i <= 8; i++) {
        s_chain_pool[i].type = BB_SERIALIZE_JSON_TOK_OBJ;
        s_chain_pool[i].child_count = 1;
        s_chain_pool[i].parent = (i == 0) ? BB_SERIALIZE_JSON_TOK_ABSENT
                                           : (bb_serialize_json_tok_idx_t)(i - 1);
        if (i > 0) {
            s_chain_pool[i].key_len = 1;
            s_chain_pool[i].key[0] = 'n';
        }
    }

    s_chain_pool[9].type = BB_SERIALIZE_JSON_TOK_ARR;
    s_chain_pool[9].parent = 8;
    s_chain_pool[9].child_count = 0;
    s_chain_pool[9].key_len = 3;
    memcpy(s_chain_pool[9].key, "arr", 3);
}

typedef struct {
    bb_serialize_arr_t arr;  // never actually written -- begin_arr fails closed first
} chain_snap_t;

// Self-referential -- "n" (OBJ) recurses into this SAME array, mirroring
// bb_serialize_populate.c's own s_deep_desc/s_deep_arr_desc probe pattern.
// "arr" listed FIRST so it is attempted (and the adapter's own guard fires)
// before "n" reaches core's pre-existing OBJ depth check one field later.
static const bb_serialize_field_t s_chain_fields[] = {
    { .key = "arr", .type = BB_TYPE_ARR, .offset = 0,
      .elem_type = BB_TYPE_STR, .max_len = 4, .max_items = 1 },
    { .key = "n", .type = BB_TYPE_OBJ, .offset = 0,
      .children = s_chain_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_chain_desc = {
    .type_name = "chain_snap_t", .fields = s_chain_fields, .n_fields = 2,
    .snap_size = sizeof(chain_snap_t),
};

void test_bb_serialize_json_populate_depth_boundary_guard_fails_closed(void)
{
    bb_serialize_json_tok_recorder_t rec;
    build_depth_chain_rec(&rec);

    struct {
        bb_serialize_json_populate_ctx_t ctx;
        uint32_t canary;
    } guard;
    memset(&guard, 0, sizeof(guard));
    guard.canary = 0xDEADBEEFu;

    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&guard.ctx, &rec);

    chain_snap_t dst = { 0 };
    bb_err_t rc = bb_serialize_populate(&s_chain_desc, &dst, &src);

    // Fail-closed, either signal is safe -- what matters is the canary.
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, guard.canary);
}

// ---------------------------------------------------------------------------
// 10b. populate_begin_obj's OWN guard TRUE branch -- bb_serialize_populate()
// itself gates BB_TYPE_OBJ nesting BEFORE ever calling begin_obj (see
// bb_serialize_populate.c), so its current OBJ path can never actually
// drive this adapter's own OBJ guard to the reject case end-to-end -- only
// the ARR guard is reachable that way (see the test above). To pin the
// adapter's OBJ guard as self-safe too (independent of that pre-existing
// core check, same defense-in-depth reasoning), this drives the returned
// bb_serialize_populate_t vtable's begin_obj directly, bypassing
// bb_serialize_populate() entirely -- a legitimate white-box use of the
// adapter's public seam. 9 real chained OBJ tokens beyond the root: the
// first 8 calls succeed (the legitimate full-depth case); the 9th finds a
// real OBJ token too (tok_is_obj true) but is rejected by the adapter's own
// depth guard before any frame-stack write.
// ---------------------------------------------------------------------------

static bb_serialize_json_tok_t s_obj_chain_pool[10];

// root(0) -> 1 -> ... -> 9, each linked by key "n" -- 9 nested OBJ tokens
// beyond the root, one past the legitimate 8-deep case.
static void build_obj_chain_rec(bb_serialize_json_tok_recorder_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    memset(s_obj_chain_pool, 0, sizeof(s_obj_chain_pool));
    rec->buf = "";
    rec->buf_len = 0;
    rec->pool = s_obj_chain_pool;
    rec->pool_cap = 10;
    rec->pool_n = 10;

    for (int i = 0; i <= 9; i++) {
        s_obj_chain_pool[i].type = BB_SERIALIZE_JSON_TOK_OBJ;
        s_obj_chain_pool[i].child_count = (i == 9) ? 0 : 1;
        s_obj_chain_pool[i].parent = (i == 0) ? BB_SERIALIZE_JSON_TOK_ABSENT
                                               : (bb_serialize_json_tok_idx_t)(i - 1);
        if (i > 0) {
            s_obj_chain_pool[i].key_len = 1;
            s_obj_chain_pool[i].key[0] = 'n';
        }
    }
}

void test_bb_serialize_json_populate_begin_obj_guard_fails_closed_past_max_depth(void)
{
    bb_serialize_json_tok_recorder_t rec;
    build_obj_chain_rec(&rec);

    struct {
        bb_serialize_json_populate_ctx_t ctx;
        uint32_t canary;
    } guard;
    memset(&guard, 0, sizeof(guard));
    guard.canary = 0xDEADBEEFu;

    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&guard.ctx, &rec);

    for (int i = 0; i < BB_SERIALIZE_MAX_DEPTH; i++) {
        TEST_ASSERT_TRUE_MESSAGE(src.begin_obj(src.ctx, "n"), "expected push to succeed within depth budget");
    }
    // The 9th nested "n" token genuinely exists (tok_is_obj is true) -- only
    // the adapter's own depth guard, not a missing token, can reject it.
    TEST_ASSERT_FALSE(src.begin_obj(src.ctx, "n"));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, guard.canary);
}

// ---------------------------------------------------------------------------
// 10c. populate_begin_arr's OWN guard TRUE branch, driven independently of
// core gating -- reuses build_depth_chain_rec() above (root + 8 chained OBJ
// tokens "n", plus one ARR token "arr" nested under the deepest OBJ, token
// 8). This drives the returned vtable's begin_obj/begin_arr DIRECTLY
// (white-box, bypassing bb_serialize_populate() and any core pre-flight), so
// the ARR guard's true-branch stays reachable even after a future core
// change adds a pre-call depth gate for begin_arr (this adapter's own
// defense-in-depth, independent of whatever core does -- see
// populate_begin_arr's doc comment in bb_serialize_json_populate.c). 8
// successful begin_obj pushes land the top frame at the last valid index
// (BB_SERIALIZE_MAX_DEPTH); the following begin_arr finds a real ARR token
// too (tok_is_arr true) but is rejected by the adapter's own depth guard
// BEFORE *count is ever written -- pinning the guard-before-count-write
// reorder (the caller's count sentinel must survive untouched on a
// fail-closed rejection). A canary immediately after the ctx's opaque
// _state[] buffer proves no OOB write to stack[MAX_DEPTH+1] either.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_populate_begin_arr_guard_fails_closed_past_max_depth(void)
{
    bb_serialize_json_tok_recorder_t rec;
    build_depth_chain_rec(&rec);

    struct {
        bb_serialize_json_populate_ctx_t ctx;
        uint32_t canary;
    } guard;
    memset(&guard, 0, sizeof(guard));
    guard.canary = 0xDEADBEEFu;

    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&guard.ctx, &rec);

    for (int i = 0; i < BB_SERIALIZE_MAX_DEPTH; i++) {
        TEST_ASSERT_TRUE_MESSAGE(src.begin_obj(src.ctx, "n"), "expected push to succeed within depth budget");
    }

    size_t count = 0xABCDu;
    // The "arr" token genuinely exists (tok_is_arr is true) -- only the
    // adapter's own depth guard, not a missing token, can reject it.
    TEST_ASSERT_FALSE(src.begin_arr(src.ctx, "arr", &count));
    TEST_ASSERT_EQUAL_UINT(0xABCDu, count);  // untouched -- guard fires before *count write
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, guard.canary);
}

// ---------------------------------------------------------------------------
// 11. get_str bounds -- a value exactly `cap` (== field max_len) bytes long
// writes the full unbounded-but-in-range prefix with no overrun; a value
// LONGER than `cap` truncates safely. A canary immediately after the dst
// struct proves neither case writes past it, and strnlen(p, max_len) (the
// documented read-side contract, see bb_serialize.h's BB_TYPE_STR doc)
// never runs past max_len even on the exactly-full, never-NUL-terminated
// case.
// ---------------------------------------------------------------------------

typedef struct {
    char name[8];
} str_bounds_snap_t;

static const bb_serialize_field_t s_str_bounds_fields[] = {
    { .key = "name", .type = BB_TYPE_STR, .offset = offsetof(str_bounds_snap_t, name),
      .max_len = sizeof(((str_bounds_snap_t *)0)->name) },
};

static const bb_serialize_desc_t s_str_bounds_desc = {
    .type_name = "str_bounds_snap_t", .fields = s_str_bounds_fields, .n_fields = 1,
    .snap_size = sizeof(str_bounds_snap_t),
};

void test_bb_serialize_json_populate_get_str_exact_cap_no_overrun(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"name\":\"12345678\"}"));  // exactly 8 == cap

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    struct {
        str_bounds_snap_t dst;
        uint32_t          canary;
    } guard;
    memset(&guard, 0, sizeof(guard));
    guard.canary = 0xDEADBEEFu;

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_str_bounds_desc, &guard.dst, &src));

    TEST_ASSERT_EQUAL_MEMORY("12345678", guard.dst.name, 8);
    TEST_ASSERT_EQUAL_UINT(8, strnlen(guard.dst.name, sizeof(guard.dst.name)));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, guard.canary);
}

void test_bb_serialize_json_populate_get_str_over_cap_truncates(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"name\":\"123456789abc\"}"));  // 12 > cap 8

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    struct {
        str_bounds_snap_t dst;
        uint32_t          canary;
    } guard;
    memset(&guard, 0, sizeof(guard));
    guard.canary = 0xDEADBEEFu;

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_str_bounds_desc, &guard.dst, &src));

    TEST_ASSERT_EQUAL_MEMORY("12345678", guard.dst.name, 8);  // safe truncated prefix
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, guard.canary);
}

// ---------------------------------------------------------------------------
// 12. Scalar type-mismatch, PRESENT key -- a JSON string value at a key each
// scalar descriptor field expects as i64/u64/f64/bool. Distinct code path
// from an absent key: the token IS found (tok_obj_get succeeds), but the
// typed accessor (tok_get_i64/u64/f64/bool) rejects it as the wrong token
// type, so the getter returns false and dst stays at its pre-populate
// sentinel -- same as absence, but via a different branch.
// ---------------------------------------------------------------------------

static const bb_serialize_desc_t s_pos_flat_desc = {
    .type_name = "pos_snap_t", .fields = s_pos_fields, .n_fields = 4, .snap_size = sizeof(pos_snap_t),
};

void test_bb_serialize_json_populate_scalar_wrong_type_present_leaves_untouched(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec,
        "{\"id\":\"x\",\"seq\":\"x\",\"temp\":\"x\",\"armed\":\"x\"}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    pos_snap_t dst = { .id = -1, .seq = 99, .temp = -1.0, .armed = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_pos_flat_desc, &dst, &src));

    TEST_ASSERT_EQUAL_INT64(-1, dst.id);
    TEST_ASSERT_EQUAL_UINT64(99, dst.seq);
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, dst.temp);
    TEST_ASSERT_TRUE(dst.armed);
}

// ---------------------------------------------------------------------------
// 13. Duplicate `.key` -- TWO fields in the SAME table bound to the SAME
// wire key ("n"), one as BB_TYPE_I64 and one as BB_TYPE_F64, pinning
// bb_serialize.h's documented "populate/ingress duplicate-key" contract
// (see its field-table doc comment, and platform/espidf/bb_system/
// bb_system_routes.c's s_reboot_fields[] for the real consumer that relies
// on it, B1-1148 finding 3). Each getter does its own independent
// first-match lookup of "n" -- both fields resolve the SAME source value,
// and table order must not matter, so this is run TWICE with the two
// duplicate fields declared in opposite order.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n_i64;
    double  n_f64;
} dup_key_snap_t;

static const bb_serialize_field_t s_dup_key_fields_i64_first[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(dup_key_snap_t, n_i64) },
    { .key = "n", .type = BB_TYPE_F64, .offset = offsetof(dup_key_snap_t, n_f64) },
};
static const bb_serialize_desc_t s_dup_key_desc_i64_first = {
    .type_name = "dup_key_snap_t", .fields = s_dup_key_fields_i64_first, .n_fields = 2,
    .snap_size = sizeof(dup_key_snap_t),
};

static const bb_serialize_field_t s_dup_key_fields_f64_first[] = {
    { .key = "n", .type = BB_TYPE_F64, .offset = offsetof(dup_key_snap_t, n_f64) },
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(dup_key_snap_t, n_i64) },
};
static const bb_serialize_desc_t s_dup_key_desc_f64_first = {
    .type_name = "dup_key_snap_t", .fields = s_dup_key_fields_f64_first, .n_fields = 2,
    .snap_size = sizeof(dup_key_snap_t),
};

void test_bb_serialize_json_populate_duplicate_key_resolves_independently(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"n\":42}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    dup_key_snap_t dst = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_dup_key_desc_i64_first, &dst, &src));
    TEST_ASSERT_EQUAL_INT64(42, dst.n_i64);
    TEST_ASSERT_EQUAL_DOUBLE(42.0, dst.n_f64);
}

void test_bb_serialize_json_populate_duplicate_key_order_irrelevant(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"n\":42}"));

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    dup_key_snap_t dst = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_dup_key_desc_f64_first, &dst, &src));
    TEST_ASSERT_EQUAL_INT64(42, dst.n_i64);
    TEST_ASSERT_EQUAL_DOUBLE(42.0, dst.n_f64);
}

// ---------------------------------------------------------------------------
// 14. B1-1167 -- populate_get_str()'s NUL-termination contract, pinned
// directly against a confirm[32]-shaped destination. populate's get_str()
// itself NEVER terminates (see bb_serialize_json_populate.c's
// populate_get_str doc comment): it copies min(value_len, max_len) bytes
// and relies entirely on the destination's own slack byte for termination.
// That makes the CORRECT max_len for a BB_TYPE_STR field direction-
// dependent, not a single universal value:
//   - INGRESS (populate) consumers that read the destination via an
//     unbounded C-string function (strcmp/strlen/etc, never a
//     strnlen()-bounded read) MUST declare max_len == sizeof(dest) - 1, so
//     a memset0-seeded destination (the BB_DATA_APPLY_POST/PATCH contract
//     every production apply route relies on) keeps its last byte '\0' no
//     matter how long the incoming value is.
//   - EGRESS (bb_serialize_walk) fields, and any ingress field the
//     consumer is careful to read only via a strnlen()-bounded scan, may
//     correctly declare the FULL sizeof(dest) instead -- strnlen(p,
//     max_len) never runs past its own bound regardless of termination.
// This suite pins two distinct paths through that same shape: n = len
// (input shorter than max_len, no truncation) and n = cap (input at/over
// max_len, truncated). Both additionally pin populate_get_str()'s
// "touches ONLY its bounded prefix" property directly: dst is pre-filled
// with a non-zero sentinel (NOT the caller-zeroed production contract --
// a deliberate white-box probe) so any write strictly after the written
// prefix, including a hypothetical defensive terminator write at dst[n],
// is observable as a change away from the sentinel. That's what actually
// distinguishes "never touches beyond n" from "writes its own NUL at
// position n" -- a plain post-populate NUL-search can't tell those apart
// when dst started pre-zeroed, since both leave the same zero byte there.
// ---------------------------------------------------------------------------

typedef struct {
    char confirm[32];
} confirm_snap_t;

static const bb_serialize_field_t s_confirm_fields[] = {
    { .key = "confirm", .type = BB_TYPE_STR, .offset = offsetof(confirm_snap_t, confirm),
      .max_len = sizeof(((confirm_snap_t *)0)->confirm) - 1 },
};

static const bb_serialize_desc_t s_confirm_desc = {
    .type_name = "confirm_snap_t", .fields = s_confirm_fields, .n_fields = 1,
    .snap_size = sizeof(confirm_snap_t),
};

#define CONFIRM_SENTINEL 0xABu

// A value shorter than max_len (31) -- n = len, no truncation. Pins that
// populate_get_str() writes ONLY those n bytes: every byte strictly after
// the written prefix, all the way to the end of the destination, is left
// completely untouched.
void test_bb_serialize_json_populate_confirm_under_cap_value_leaves_tail_untouched(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"confirm\":\"short-value\"}"));  // 11 bytes, < 31

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    confirm_snap_t dst;
    memset(&dst, CONFIRM_SENTINEL, sizeof(dst));

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_confirm_desc, &dst, &src));

    TEST_ASSERT_EQUAL_MEMORY("short-value", dst.confirm, 11);
    for (size_t i = 11; i < sizeof(dst.confirm); i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(CONFIRM_SENTINEL, (uint8_t)dst.confirm[i],
            "byte strictly after the written prefix was touched -- populate_get_str() must write "
            "ONLY its bounded prefix, never a trailing terminator of its own");
    }
}

// A value at/over max_len (31) -- n = cap, the truncating path (content
// already covered by case 11 above). Its distinct value here is narrower
// than the under-cap case above: under the "defensive terminator only
// when room remains" mutant, n == cap so that guard never fires and this
// test passes regardless -- it does NOT pin "never touches beyond n" the
// way the under-cap case does. What it DOES pin is real: the ONE
// remaining byte beyond the written cap is dst.confirm's designed slack
// byte for THIS ingress shape (max_len == sizeof(dst) - 1), so this is
// the only test proving no write lands on that same-buffer slack byte
// under truncation -- distinct from the pre-existing
// test_bb_serialize_json_populate_get_str_over_cap_truncates (case 11),
// whose egress-style zero-slack buffer (max_len == full sizeof) can only
// prove no overrun into an ADJACENT field's canary, never a same-buffer
// slack byte, since that shape has none. NUL termination after truncation
// still comes entirely from the caller's own pre-zeroing (BB_DATA_APPLY_POST's
// memset0 seed in production), never from populate_get_str() itself.
void test_bb_serialize_json_populate_confirm_over_cap_value_truncates_and_leaves_tail_untouched(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec,
        "{\"confirm\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}"));  // 40 'x', > cap (31)

    bb_serialize_json_populate_ctx_t ctx;
    bb_serialize_populate_t src = bb_serialize_json_populate_from_tok(&ctx, &rec);

    confirm_snap_t dst;
    memset(&dst, CONFIRM_SENTINEL, sizeof(dst));

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_confirm_desc, &dst, &src));

    TEST_ASSERT_EQUAL_MEMORY("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", dst.confirm, 31);  // 31 'x' == cap
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(CONFIRM_SENTINEL, (uint8_t)dst.confirm[31],
        "byte strictly after the written prefix was touched -- populate_get_str() must write "
        "ONLY its bounded prefix, never a trailing terminator of its own");
}
