#include "unity.h"
#include "bb_data.h"
#include "bb_serialize_console.h"
#include "bb_serialize_format.h"
#include "bb_task.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// bb_serialize_console_render -- golden "key=val key=val" line + every
// scalar emit path.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t               i;
    uint64_t              u;
    double                f;
    bool                  b;
    bb_serialize_str_n_t  s;
} console_flat_snap_t;

static const bb_serialize_field_t s_console_flat_fields[] = {
    { .key = "i", .type = BB_TYPE_I64, .offset = offsetof(console_flat_snap_t, i) },
    { .key = "u", .type = BB_TYPE_U64, .offset = offsetof(console_flat_snap_t, u) },
    { .key = "f", .type = BB_TYPE_F64, .offset = offsetof(console_flat_snap_t, f) },
    { .key = "b", .type = BB_TYPE_BOOL, .offset = offsetof(console_flat_snap_t, b) },
    { .key = "s", .type = BB_TYPE_STR_N, .offset = offsetof(console_flat_snap_t, s), .max_len = 8 },
};

static const bb_serialize_desc_t s_console_flat_desc = {
    .type_name = "console_flat_snap_t",
    .fields = s_console_flat_fields,
    .n_fields = 5,
    .snap_size = sizeof(console_flat_snap_t),
};

void test_bb_serialize_console_flat_scalars(void)
{
    console_flat_snap_t snap = {
        .i = -7, .u = 42, .f = 1.5, .b = true, .s = { .ptr = "hi", .len = 2 },
    };
    char buf[128];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_flat_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("i=-7 u=42 f=1.5 b=true s=hi", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

void test_bb_serialize_console_null_str_n_emits_null(void)
{
    console_flat_snap_t snap = {
        .i = 0, .u = 0, .f = 0.0, .b = false, .s = { .ptr = NULL, .len = 0 },
    };
    char buf[128];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_flat_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("i=0 u=0 f=0 b=false s=null", buf);
}

void test_bb_serialize_console_empty_str_n_emits_empty(void)
{
    console_flat_snap_t snap = {
        .i = 0, .u = 0, .f = 0.0, .b = false, .s = { .ptr = "", .len = 0 },
    };
    char buf[128];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_flat_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("i=0 u=0 f=0 b=false s=", buf);
}

// ---------------------------------------------------------------------------
// Nested OBJ/ARR: structurally safe (never crashes), begin/end are no-ops.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} console_inner_t;

typedef struct {
    console_inner_t o;
} console_obj_snap_t;

static const bb_serialize_field_t s_console_inner_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(console_inner_t, n) },
};

static const bb_serialize_field_t s_console_obj_fields[] = {
    { .key = "o", .type = BB_TYPE_OBJ, .offset = offsetof(console_obj_snap_t, o),
      .children = s_console_inner_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_console_obj_desc = {
    .type_name = "console_obj_snap_t",
    .fields = s_console_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(console_obj_snap_t),
};

void test_bb_serialize_console_nested_obj_no_crash(void)
{
    console_obj_snap_t snap = { .o = { .n = 5 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_obj_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    // Nesting-aware key qualification (B1-1416): the child's key is
    // prefixed with its parent's key, dotted -- no brackets. THIS IS
    // LOAD-BEARING: a path-stack mechanism that pushes/pops but is never
    // read at emit time reproduces the old flat "n=5" output, and only
    // this assertion catches that.
    TEST_ASSERT_EQUAL_STRING("o.n=5", buf);
}

// ---------------------------------------------------------------------------
// Nested-key qualification (B1-1416) -- multi-level joining, sibling
// disambiguation, depth-cap, and truncation interaction.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t c;
} console_abc_level_c_t;

typedef struct {
    console_abc_level_c_t b;
} console_abc_level_b_t;

typedef struct {
    console_abc_level_b_t a;
} console_abc_snap_t;

static const bb_serialize_field_t s_console_abc_c_fields[] = {
    { .key = "c", .type = BB_TYPE_I64, .offset = offsetof(console_abc_level_c_t, c) },
};
static const bb_serialize_field_t s_console_abc_b_fields[] = {
    { .key = "b", .type = BB_TYPE_OBJ, .offset = offsetof(console_abc_level_b_t, b),
      .children = s_console_abc_c_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_console_abc_a_fields[] = {
    { .key = "a", .type = BB_TYPE_OBJ, .offset = offsetof(console_abc_snap_t, a),
      .children = s_console_abc_b_fields, .n_children = 1 },
};
static const bb_serialize_desc_t s_console_abc_desc = {
    .type_name = "console_abc_snap_t",
    .fields = s_console_abc_a_fields,
    .n_fields = 1,
    .snap_size = sizeof(console_abc_snap_t),
};

void test_bb_serialize_console_two_level_nesting_joins_full_path(void)
{
    console_abc_snap_t snap = { .a = { .b = { .c = 1 } } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_abc_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("a.b.c=1", buf);
}

typedef struct {
    int64_t free;
} console_region_t;

typedef struct {
    console_region_t r1;
    console_region_t r2;
} console_regions_snap_t;

static const bb_serialize_field_t s_console_region_fields[] = {
    { .key = "free", .type = BB_TYPE_I64, .offset = offsetof(console_region_t, free) },
};
static const bb_serialize_field_t s_console_regions_fields[] = {
    { .key = "r1", .type = BB_TYPE_OBJ, .offset = offsetof(console_regions_snap_t, r1),
      .children = s_console_region_fields, .n_children = 1 },
    { .key = "r2", .type = BB_TYPE_OBJ, .offset = offsetof(console_regions_snap_t, r2),
      .children = s_console_region_fields, .n_children = 1 },
};
static const bb_serialize_desc_t s_console_regions_desc = {
    .type_name = "console_regions_snap_t",
    .fields = s_console_regions_fields,
    .n_fields = 2,
    .snap_size = sizeof(console_regions_snap_t),
};

// THE MOTIVATING CASE: two sibling BB_TYPE_OBJ children sharing an
// identical child key ("free") render as distinct, disambiguated tokens
// rather than colliding on one line (the bug this ticket fixes -- see
// bb_meminfo_heap_snap_desc's per-region objects, the real-world trigger).
void test_bb_serialize_console_sibling_objects_disambiguate_shared_child_key(void)
{
    console_regions_snap_t snap = { .r1 = { .free = 1 }, .r2 = { .free = 2 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_regions_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("r1.free=1 r2.free=2", buf);
}

// Depth-cap defensive case: an 8-level-deep nested descriptor (matching
// BB_SERIALIZE_MAX_DEPTH exactly) proves the path stack tracks the
// walker's own recursion cap without corrupting path[] -- the walker
// itself refuses to recurse past this depth (bb_serialize_walk.c), so this
// exercises the boundary the path stack must never exceed.
typedef struct { int64_t v; } console_deep8_t;
typedef struct { console_deep8_t l8; } console_deep7_t;
typedef struct { console_deep7_t l7; } console_deep6_t;
typedef struct { console_deep6_t l6; } console_deep5_t;
typedef struct { console_deep5_t l5; } console_deep4_t;
typedef struct { console_deep4_t l4; } console_deep3_t;
typedef struct { console_deep3_t l3; } console_deep2_t;
typedef struct { console_deep2_t l2; } console_deep1_snap_t;

static const bb_serialize_field_t s_console_deep8_fields[] = {
    { .key = "v", .type = BB_TYPE_I64, .offset = offsetof(console_deep8_t, v) },
};
static const bb_serialize_field_t s_console_deep7_fields[] = {
    { .key = "l8", .type = BB_TYPE_OBJ, .offset = offsetof(console_deep7_t, l8),
      .children = s_console_deep8_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_console_deep6_fields[] = {
    { .key = "l7", .type = BB_TYPE_OBJ, .offset = offsetof(console_deep6_t, l7),
      .children = s_console_deep7_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_console_deep5_fields[] = {
    { .key = "l6", .type = BB_TYPE_OBJ, .offset = offsetof(console_deep5_t, l6),
      .children = s_console_deep6_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_console_deep4_fields[] = {
    { .key = "l5", .type = BB_TYPE_OBJ, .offset = offsetof(console_deep4_t, l5),
      .children = s_console_deep5_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_console_deep3_fields[] = {
    { .key = "l4", .type = BB_TYPE_OBJ, .offset = offsetof(console_deep3_t, l4),
      .children = s_console_deep4_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_console_deep2_fields[] = {
    { .key = "l3", .type = BB_TYPE_OBJ, .offset = offsetof(console_deep2_t, l3),
      .children = s_console_deep3_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_console_deep1_fields[] = {
    { .key = "l2", .type = BB_TYPE_OBJ, .offset = offsetof(console_deep1_snap_t, l2),
      .children = s_console_deep2_fields, .n_children = 1 },
};
static const bb_serialize_desc_t s_console_deep1_desc = {
    .type_name = "console_deep1_snap_t",
    .fields = s_console_deep1_fields,
    .n_fields = 1,
    .snap_size = sizeof(console_deep1_snap_t),
};

// Nests 7 levels deep (l2..l8), so path_depth reaches 7 -- one short of
// BB_SERIALIZE_MAX_DEPTH (8) -- and joins the full dotted path cleanly.
// This proves deep nesting UNDER the cap, not AT or past it: the
// push-skip-on-cap guard's true arm in bb_console_push_path() is
// unreachable from outside the walker by construction, since
// bb_serialize_walk.c's own `depth >= BB_SERIALIZE_MAX_DEPTH` check (see
// bb_serialize_walk.c) breaks BEFORE ever calling begin_obj at depth 8 --
// there is no legitimate walk that can drive this backend's guard to its
// true arm (see that guard's LCOV_EXCL_LINE in bb_serialize_console.c).
void test_bb_serialize_console_deep_nesting_under_cap_joins_full_path(void)
{
    console_deep1_snap_t snap = { .l2 = { .l3 = { .l4 = { .l5 = { .l6 = { .l7 = { .l8 = { .v = 42 } } } } } } } };
    char buf[128];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_deep1_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("l2.l3.l4.l5.l6.l7.l8.v=42", buf);
}

// Truncation interaction: a qualified line overflowing cap still truncates
// cleanly and stays NUL-terminated -- the same snprintf-style contract as
// every other emit path in this backend, now exercised through the
// path-join loop in bb_console_pre_value() rather than a single key write.
void test_bb_serialize_console_nested_key_truncates_cleanly(void)
{
    console_regions_snap_t snap = { .r1 = { .free = 1 }, .r2 = { .free = 2 } };
    char buf[10];  // "r1.free=1" is 9 chars + NUL == exactly 10; cap below forces a clip
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_regions_desc, &snap, buf, 6, &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    // Pins the exact clip point, not just structural NUL-termination: the
    // untruncated line is "r1.free=1", cap 6 leaves room for "r1." (3) plus
    // a clipped "fr" (2, one shy of the NUL) from the "free=" key -- the
    // value digit never gets written. Truncating mid-key like this yields
    // an ambiguous half-key rather than a truncated value, but that's
    // pre-existing truncate-don't-fail backend behaviour (applied
    // consistently across every emit path in this file), not something
    // B1-1416 introduced -- not changed here.
    TEST_ASSERT_EQUAL_STRING("r1.fr", buf);
    TEST_ASSERT_EQUAL('\0', buf[5]);
    TEST_ASSERT_TRUE(out_len < 6);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

// Array-of-obj elements are begin_obj'd with key == NULL (see
// bb_serialize_walk.c) -- pushed onto the path stack like any other OBJ
// (balanced push/pop) but skipped when joining a qualified key, so a
// scalar OBJ-nested *inside* an array-of-obj row still qualifies against
// its own ancestor keys, just without an array-row segment prefixed.
// Exercises bb_console_pre_value()'s `if (ctx->path[i])` skip-NULL branch.
typedef struct {
    int64_t v;
} console_arr_row_inner_t;

typedef struct {
    console_arr_row_inner_t inner;
} console_arr_row_t;

typedef struct {
    bb_serialize_arr_t rows;
} console_arr_of_obj_snap_t;

static const bb_serialize_field_t s_console_arr_row_inner_fields[] = {
    { .key = "v", .type = BB_TYPE_I64, .offset = offsetof(console_arr_row_inner_t, v) },
};
static const bb_serialize_field_t s_console_arr_row_fields[] = {
    { .key = "inner", .type = BB_TYPE_OBJ, .offset = offsetof(console_arr_row_t, inner),
      .children = s_console_arr_row_inner_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_console_arr_of_obj_fields[] = {
    { .key = "rows", .type = BB_TYPE_ARR, .offset = offsetof(console_arr_of_obj_snap_t, rows),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(console_arr_row_t), .max_items = 4,
      .children = s_console_arr_row_fields, .n_children = 1 },
};
static const bb_serialize_desc_t s_console_arr_of_obj_desc = {
    .type_name = "console_arr_of_obj_snap_t",
    .fields = s_console_arr_of_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(console_arr_of_obj_snap_t),
};

void test_bb_serialize_console_array_of_obj_row_qualifies_without_row_key(void)
{
    console_arr_row_t rows[1] = { { .inner = { .v = 1 } } };
    console_arr_of_obj_snap_t snap = { .rows = { .items = rows, .count = 1 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_arr_of_obj_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("inner.v=1", buf);
}

typedef struct {
    bb_serialize_arr_t a;
} console_arr_snap_t;

static const bb_serialize_field_t s_console_arr_fields[] = {
    { .key = "a", .type = BB_TYPE_ARR, .offset = offsetof(console_arr_snap_t, a),
      .elem_type = BB_TYPE_STR, .max_len = 8, .max_items = 4 },
};

static const bb_serialize_desc_t s_console_arr_desc = {
    .type_name = "console_arr_snap_t",
    .fields = s_console_arr_fields,
    .n_fields = 1,
    .snap_size = sizeof(console_arr_snap_t),
};

void test_bb_serialize_console_array_of_strings_no_crash(void)
{
    const char *items[] = { "x", "y" };
    console_arr_snap_t snap = { .a = { .items = items, .count = 2 } };
    char buf[64];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_arr_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("x y", buf);
}

// ---------------------------------------------------------------------------
// Truncation-on-overflow -- snprintf semantics, always NUL-terminated,
// always BB_OK (never BB_ERR_NO_SPACE for a non-degenerate buf/cap).
// ---------------------------------------------------------------------------

void test_bb_serialize_console_render_truncates_cleanly(void)
{
    console_flat_snap_t snap = {
        .i = 123456789, .u = 42, .f = 1.5, .b = true, .s = { .ptr = "hi", .len = 2 },
    };
    char buf[6];
    size_t out_len = 0;

    bb_err_t rc = bb_serialize_console_render(&s_console_flat_desc, &snap, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL('\0', buf[sizeof(buf) - 1]);
    TEST_ASSERT_TRUE(out_len < sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(strlen(buf), out_len);
}

void test_bb_serialize_console_render_rejects_null_buf(void)
{
    console_flat_snap_t snap = { 0 };
    size_t out_len = 123;

    bb_err_t rc = bb_serialize_console_render(&s_console_flat_desc, &snap, NULL, 16, &out_len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT(0, out_len);
}

void test_bb_serialize_console_render_rejects_zero_cap(void)
{
    console_flat_snap_t snap = { 0 };
    char buf[16] = { 'X' };
    size_t out_len = 123;

    bb_err_t rc = bb_serialize_console_render(&s_console_flat_desc, &snap, buf, 0, &out_len);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT(0, out_len);
    TEST_ASSERT_EQUAL('X', buf[0]);  // untouched
}

// out_len is optional (NULL-tolerant) on both the reject path and the
// success path -- exercises the `if (out_len)` guard's false arm each side.
void test_bb_serialize_console_render_rejects_zero_cap_null_out_len(void)
{
    console_flat_snap_t snap = { 0 };
    char buf[16] = { 'X' };

    bb_err_t rc = bb_serialize_console_render(&s_console_flat_desc, &snap, buf, 0, NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL('X', buf[0]);  // untouched
}

void test_bb_serialize_console_render_success_null_out_len(void)
{
    console_flat_snap_t snap = {
        .i = 1, .u = 2, .f = 3.0, .b = true, .s = { .ptr = "z", .len = 1 },
    };
    char buf[64];

    bb_err_t rc = bb_serialize_console_render(&s_console_flat_desc, &snap, buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("i=1 u=2 f=3 b=true s=z", buf);
}

// bb_serialize_console_ctx_init() is public API -- a direct cap==0 call
// (bypassing bb_serialize_console_render()'s own guard) must not write past
// a zero-capacity buffer.
void test_bb_serialize_console_ctx_init_zero_cap_does_not_write(void)
{
    char buf[1] = { 'X' };
    bb_serialize_console_ctx_t ctx;

    bb_serialize_console_ctx_init(&ctx, buf, 0);

    TEST_ASSERT_EQUAL_UINT(0, ctx.cap);
    TEST_ASSERT_EQUAL_UINT(0, ctx.len);
    TEST_ASSERT_EQUAL('X', buf[0]);  // untouched -- no room even for a NUL
}

// bb_console_appendf()'s `ctx->len + 1 > ctx->cap` guard is reachable via the
// public API: a caller can construct a zero-capacity ctx directly
// (bb_serialize_console_ctx_init(&ctx, buf, 0), bypassing
// bb_serialize_console_render()'s own cap == 0 rejection) and walk a
// descriptor through it -- the very first append (the pre_value "key="
// write) hits the guard's true arm and must return without writing.
void test_bb_serialize_console_emit_zero_cap_ctx_walk_is_noop(void)
{
    typedef struct { int64_t n; } zero_cap_snap_t;
    static const bb_serialize_field_t fields[] = {
        { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(zero_cap_snap_t, n) },
    };
    static const bb_serialize_desc_t desc = {
        .type_name = "zero_cap_snap_t", .fields = fields, .n_fields = 1,
        .snap_size = sizeof(zero_cap_snap_t),
    };
    zero_cap_snap_t snap = { .n = 7 };

    char buf[1] = { 'X' };
    bb_serialize_console_ctx_t ctx;
    bb_serialize_console_ctx_init(&ctx, buf, 0);

    bb_serialize_emit_t emit = bb_serialize_console_emit(&ctx);
    bb_serialize_walk(&(bb_serialize_walk_cfg_t){ .desc = &desc, .snap = &snap, .emit = &emit });

    TEST_ASSERT_EQUAL_UINT(0, ctx.len);
    TEST_ASSERT_EQUAL('X', buf[0]);  // untouched -- no room even for a NUL
}

// ---------------------------------------------------------------------------
// bb_serialize_console_register_format -- registry round trip + idempotency.
// ---------------------------------------------------------------------------

void test_bb_serialize_console_register_format_idempotent(void)
{
    bb_serialize_format_test_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_register_format());
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_register_format());

    bb_serialize_render_fn render = bb_serialize_format_get_render(BB_FORMAT_CONSOLE);
    TEST_ASSERT_NOT_NULL(render);
    TEST_ASSERT_NULL(bb_serialize_format_get_parse(BB_FORMAT_CONSOLE));

    typedef struct { int64_t n; } rt_snap_t;
    static const bb_serialize_field_t rt_fields[] = {
        { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(rt_snap_t, n) },
    };
    static const bb_serialize_desc_t rt_desc = {
        .type_name = "rt_snap_t", .fields = rt_fields, .n_fields = 1, .snap_size = sizeof(rt_snap_t),
    };
    rt_snap_t rt = { .n = 7 };
    char buf[32];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, render(&rt_desc, &rt, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("n=7", buf);
}

// ---------------------------------------------------------------------------
// bb_serialize_console_heap_gather / bb_serialize_console_heap_report --
// host bb_meminfo_get() zero-fills its snapshot (no heap_caps equivalent on
// host), so a host-side gather deterministically yields an all-zero
// bb_serialize_console_heap_snap_t.
// ---------------------------------------------------------------------------

void test_bb_serialize_console_heap_gather_rejects_null_dst(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_console_heap_gather(NULL, NULL));
}

void test_bb_serialize_console_heap_gather_host_zero_snapshot(void)
{
    bb_serialize_console_heap_snap_t snap;
    memset(&snap, 0xAA, sizeof(snap));

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_heap_gather(&snap, NULL));

    TEST_ASSERT_EQUAL_UINT64(0, snap.internal_free);
    TEST_ASSERT_EQUAL_UINT64(0, snap.internal_min_ever_free);
    TEST_ASSERT_EQUAL_UINT64(0, snap.internal_largest_free_block);
    TEST_ASSERT_EQUAL_UINT64(0, snap.spiram_free);
    TEST_ASSERT_EQUAL_UINT64(0, snap.dma_free);
    TEST_ASSERT_EQUAL_UINT64(0, snap.esp_min_free_heap);
}

void test_bb_serialize_console_heap_desc_matches_snap_layout(void)
{
    TEST_ASSERT_EQUAL_STRING("bb_serialize_console_heap_snap_t", bb_serialize_console_heap_desc.type_name);
    TEST_ASSERT_EQUAL_UINT16(6, bb_serialize_console_heap_desc.n_fields);
    TEST_ASSERT_EQUAL_UINT16(sizeof(bb_serialize_console_heap_snap_t), bb_serialize_console_heap_desc.snap_size);
}

void test_bb_serialize_console_heap_report_renders_via_desc(void)
{
    bb_serialize_console_heap_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_heap_gather(&snap, NULL));

    char buf[160];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_render(&bb_serialize_console_heap_desc, &snap,
                                                          buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING(
        "internal_free=0 internal_min_ever_free=0 internal_largest_free_block=0 "
        "spiram_free=0 dma_free=0 esp_min_free_heap=0",
        buf);
}

// bb_serialize_console_heap_report() itself has no observable return value
// (it logs) -- these calls are a smoke/no-crash check for both the
// labelled and NULL-label paths; the actual line content is covered above
// via the desc+gather+render path directly.
void test_bb_serialize_console_heap_report_smoke(void)
{
    bb_serialize_console_heap_report("boot");
    bb_serialize_console_heap_report(NULL);
}

// ---------------------------------------------------------------------------
// bb_serialize_console_tasks_gather / _row_desc / _report -- reads
// bb_task's base registry (BB_TASK_TESTING seam: bb_task_base_upsert() +
// bb_task_base_set_free_bytes() seed entries directly, no real FreeRTOS
// task/scan needed).
// ---------------------------------------------------------------------------

void test_bb_serialize_console_tasks_gather_rejects_null_dst(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL_UINT(0, bb_serialize_console_tasks_gather(NULL, 4));
}

void test_bb_serialize_console_tasks_gather_rejects_zero_cap(void)
{
    bb_task_base_test_reset();
    bb_serialize_console_tasks_row_snap_t rows[1];
    TEST_ASSERT_EQUAL_UINT(0, bb_serialize_console_tasks_gather(rows, 0));
}

void test_bb_serialize_console_tasks_gather_empty_registry_yields_zero_rows(void)
{
    bb_task_base_test_reset();
    bb_serialize_console_tasks_row_snap_t rows[4];
    TEST_ASSERT_EQUAL_UINT(0, bb_serialize_console_tasks_gather(rows, 4));
}

static const bb_serialize_console_tasks_row_snap_t *find_row(
    const bb_serialize_console_tasks_row_snap_t *rows, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(rows[i].name, name) == 0) return &rows[i];
    }
    return NULL;
}

// A "known-budget" task (created via bb_task_create()/bb_task_registry_register()
// in real life, seeded here via bb_task_base_upsert()) surfaces budget/used;
// a scan-only placeholder (budget unknown, e.g. a third-party task like
// wifi_prov_mgr) surfaces free_bytes only.
void test_bb_serialize_console_tasks_gather_computes_used_bytes_when_budget_known(void)
{
    bb_task_base_test_reset();
    int known, unknown;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&known, "known", 4096, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_set_free_bytes(&known, 1000));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&unknown, "unknown", 0, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_set_free_bytes(&unknown, 500));

    bb_serialize_console_tasks_row_snap_t rows[4];
    size_t n = bb_serialize_console_tasks_gather(rows, 4);
    TEST_ASSERT_EQUAL_UINT(2, n);

    const bb_serialize_console_tasks_row_snap_t *k = find_row(rows, n, "known");
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_TRUE(k->sampled);
    TEST_ASSERT_EQUAL_UINT64(1000, k->free_bytes);
    TEST_ASSERT_EQUAL_UINT64(4096, k->stack_budget_bytes);
    TEST_ASSERT_EQUAL_UINT64(3096, k->used_bytes);

    const bb_serialize_console_tasks_row_snap_t *u = find_row(rows, n, "unknown");
    TEST_ASSERT_NOT_NULL(u);
    TEST_ASSERT_TRUE(u->sampled);
    TEST_ASSERT_EQUAL_UINT64(500, u->free_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, u->stack_budget_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, u->used_bytes);
}

// Defensive clamp: free_bytes observed greater than the known budget (a
// stale-sample edge case, see bb_serialize_console_tasks.c's gather comment)
// must clamp used_bytes to 0, never underflow.
void test_bb_serialize_console_tasks_gather_clamps_used_bytes_when_free_exceeds_budget(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "odd", 1024, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_set_free_bytes(&fake, 2048));

    bb_serialize_console_tasks_row_snap_t rows[1];
    size_t n = bb_serialize_console_tasks_gather(rows, 1);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0, rows[0].used_bytes);
}

// [HIGH review finding] Reproduces the gap the existing gather tests can't
// catch: they always call bb_task_base_set_free_bytes() before gathering.
// A task upserted but never scanned must gather with sampled==false and
// free_bytes==0, distinguishably from a genuinely-scanned low reading.
void test_bb_serialize_console_tasks_gather_marks_unsampled_task_unsampled(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "never-scanned", 4096, false));

    bb_serialize_console_tasks_row_snap_t rows[1];
    size_t n = bb_serialize_console_tasks_gather(rows, 1);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_FALSE(rows[0].sampled);
    TEST_ASSERT_EQUAL_UINT64(0, rows[0].free_bytes);
    TEST_ASSERT_EQUAL_UINT64(4096, rows[0].stack_budget_bytes);

    char buf[160];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_render(&bb_serialize_console_tasks_row_desc, &rows[0],
                                                          buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("name=never-scanned budget_bytes=4096", buf);
}

void test_bb_serialize_console_tasks_gather_truncates_at_cap(void)
{
    bb_task_base_test_reset();
    int a, b, c;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&a, "a", 0, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&b, "b", 0, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&c, "c", 0, false));

    bb_serialize_console_tasks_row_snap_t rows[2];
    TEST_ASSERT_EQUAL_UINT(2, bb_serialize_console_tasks_gather(rows, 2));
}

void test_bb_serialize_console_tasks_row_desc_matches_snap_layout(void)
{
    TEST_ASSERT_EQUAL_STRING("bb_serialize_console_tasks_row_snap_t",
                              bb_serialize_console_tasks_row_desc.type_name);
    TEST_ASSERT_EQUAL_UINT16(4, bb_serialize_console_tasks_row_desc.n_fields);
    TEST_ASSERT_EQUAL_UINT16(sizeof(bb_serialize_console_tasks_row_snap_t),
                              bb_serialize_console_tasks_row_desc.snap_size);
}

void test_bb_serialize_console_tasks_row_desc_renders_with_budget(void)
{
    bb_serialize_console_tasks_row_snap_t row = { .free_bytes = 1000, .stack_budget_bytes = 4096, .used_bytes = 3096, .sampled = true };
    strncpy(row.name, "known", sizeof(row.name) - 1);

    char buf[160];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_render(&bb_serialize_console_tasks_row_desc, &row,
                                                          buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("name=known free_bytes=1000 budget_bytes=4096 used_bytes=3096", buf);
}

void test_bb_serialize_console_tasks_row_desc_renders_without_budget(void)
{
    bb_serialize_console_tasks_row_snap_t row = { .free_bytes = 500, .sampled = true };
    strncpy(row.name, "unknown", sizeof(row.name) - 1);

    char buf[160];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_render(&bb_serialize_console_tasks_row_desc, &row,
                                                          buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("name=unknown free_bytes=500", buf);
}

// [HIGH review finding] A task that was upserted (so bb_task knows its
// name/budget) but whose free_bytes has never been written by
// bb_task_base_set_free_bytes() -- i.e. no base-scan pass has landed for it
// yet -- must render as UNMEASURED, not as a false "0 bytes free" alarm.
void test_bb_serialize_console_tasks_row_desc_renders_without_sample(void)
{
    bb_serialize_console_tasks_row_snap_t row = { .stack_budget_bytes = 4096 };
    strncpy(row.name, "unsampled", sizeof(row.name) - 1);

    char buf[160];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_render(&bb_serialize_console_tasks_row_desc, &row,
                                                          buf, sizeof(buf), &out_len));
    // budget_bytes still renders (a known, configured budget is independent
    // of whether a scan has sampled free_bytes yet) but free_bytes/used_bytes
    // do not.
    TEST_ASSERT_EQUAL_STRING("name=unsampled budget_bytes=4096", buf);
}

// ---------------------------------------------------------------------------
// bb_data_row_gather_fn thunk shape (B1-1418 PR-2) -- exercises the exact
// pattern examples/floor/main/floor_app.c's private static thunk uses to
// drive bb_serialize_console_tasks_row_desc/bb_serialize_console_tasks_gather()
// through bb_data's rows-only table-producer shape
// (bb_data_bind()/bb_data_render_rows()). floor_app.c itself is not
// host-compiled (examples aren't part of the native scaffold's component
// graph), so this pins the thunk's shape/behavior -- `args` ignored,
// delegates straight through to bb_serialize_console_tasks_gather() -- at
// the layer that IS host-tested; the thunk in floor_app.c is a one-line
// forwarder with no branches of its own to separately cover.
//
// KNOWN GAP, accepted (firmware review MEDIUM finding on B1-1418 PR-2,
// declined as out of scope for this migration): `test_tasks_row_gather_thunk`
// below is a HAND-MIRRORED COPY of floor_app.c's `tasks_row_gather`, not an
// #include of the same translation unit (unlike floor_prov_reboot.c/
// floor_task_stack.c, which test/test_host/test_floor_prov_reboot.c and
// test_floor_task_stack.c #include directly for exactly this reason --
// floor_app.c itself can't be pulled in the same way, since it drags in
// the whole composition root). This means editing floor's real thunk
// without updating this mirror to match leaves this test passing against
// STALE logic -- a real drift risk, not merely a style duplication. Restructuring
// the example/host boundary so floor_app.c's own thunk becomes directly
// host-includable (mirroring the floor_prov_reboot.c/floor_task_stack.c
// split) is deferred to its own ticket, not folded into this migration PR.
// ---------------------------------------------------------------------------

static size_t test_tasks_row_gather_thunk(void *dst_rows, size_t max_rows, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_serialize_console_tasks_gather((bb_serialize_console_tasks_row_snap_t *)dst_rows, max_rows);
}

static const bb_data_row_binding_t s_test_tasks_row_binding = {
    .row_desc   = &bb_serialize_console_tasks_row_desc,
    .row_gather = test_tasks_row_gather_thunk,
    .row_size   = sizeof(bb_serialize_console_tasks_row_snap_t),
};

#define TEST_TASKS_ROW_CAP 4
static bb_serialize_console_tasks_row_snap_t s_test_tasks_row_scratch[TEST_TASKS_ROW_CAP];

typedef struct {
    char   line[160];
    size_t len;
    size_t row_idx;
} test_tasks_captured_row_t;

static test_tasks_captured_row_t s_test_tasks_captured[TEST_TASKS_ROW_CAP];
static size_t                    s_test_tasks_captured_count = 0;

static void test_tasks_emit_capture(const char *line, size_t len, size_t row_idx, void *ctx)
{
    (void)ctx;
    if (s_test_tasks_captured_count >= TEST_TASKS_ROW_CAP) return;

    test_tasks_captured_row_t *cap = &s_test_tasks_captured[s_test_tasks_captured_count++];
    strncpy(cap->line, line, sizeof(cap->line) - 1);
    cap->line[sizeof(cap->line) - 1] = '\0';
    cap->len                          = len;
    cap->row_idx                      = row_idx;
}

// The whole point: a rows-only "tasks"-style binding (desc/gather both NULL,
// rows-only, exactly floor's shape) built off the REAL production
// row_desc/gather pair, rendered end to end through
// bb_data_render_rows() -- content asserted, not just a return-code smoke
// check.
void test_bb_data_render_rows_tasks_binding_renders_content(void)
{
    bb_task_base_test_reset();
    bb_data_test_reset();
    bb_serialize_format_test_reset();
    s_test_tasks_captured_count = 0;
    memset(s_test_tasks_captured, 0, sizeof(s_test_tasks_captured));

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_register_format());

    bb_data_binding_t binding = {
        .key = "test.tasks", .desc = NULL, .gather = NULL, .rows = &s_test_tasks_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&binding));

    int worker;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&worker, "worker", 2048, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_set_free_bytes(&worker, 1500));

    bb_data_render_rows_req_t req = {
        .key = "test.tasks", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_test_tasks_row_scratch, .max_rows = TEST_TASKS_ROW_CAP,
        .emit_row = test_tasks_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));

    TEST_ASSERT_EQUAL_UINT(1, s_test_tasks_captured_count);
    TEST_ASSERT_EQUAL_UINT(0, s_test_tasks_captured[0].row_idx);
    TEST_ASSERT_EQUAL_STRING("name=worker free_bytes=1500 budget_bytes=2048 used_bytes=548",
                              s_test_tasks_captured[0].line);
}

// Empty registry: bb_data_render_rows() still returns BB_OK with zero rows
// emitted (mirrors the retired bb_serialize_console_tasks_report()'s own
// "empty registry logs nothing, not an error" contract).
void test_bb_data_render_rows_tasks_binding_empty_registry_emits_nothing(void)
{
    bb_task_base_test_reset();
    bb_data_test_reset();
    bb_serialize_format_test_reset();
    s_test_tasks_captured_count = 0;
    memset(s_test_tasks_captured, 0, sizeof(s_test_tasks_captured));

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_console_register_format());

    bb_data_binding_t binding = {
        .key = "test.tasks.empty", .desc = NULL, .gather = NULL, .rows = &s_test_tasks_row_binding,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&binding));

    bb_data_render_rows_req_t req = {
        .key = "test.tasks.empty", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_test_tasks_row_scratch, .max_rows = TEST_TASKS_ROW_CAP,
        .emit_row = test_tasks_emit_capture,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render_rows(&req));
    TEST_ASSERT_EQUAL_UINT(0, s_test_tasks_captured_count);
}

// ---------------------------------------------------------------------------
// bb_format_name -- BB_FORMAT_CONSOLE coverage (BB_FORMAT_JSON/NONE/
// out-of-range already covered by test_bb_serialize_format.c).
// ---------------------------------------------------------------------------

void test_bb_format_name_console_returns_console(void)
{
    TEST_ASSERT_EQUAL_STRING("console", bb_format_name(BB_FORMAT_CONSOLE));
}
