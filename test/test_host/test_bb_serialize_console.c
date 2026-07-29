#include "unity.h"
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
    // No nesting-aware key qualification -- the child's own key is emitted
    // flat (no parent-key prefix, no brackets).
    TEST_ASSERT_EQUAL_STRING("n=5", buf);
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
    bb_serialize_walk(&desc, &snap, &emit);

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

// bb_serialize_console_tasks_report() itself has no observable return value
// (it logs, one line per task) -- smoke/no-crash check for the empty
// registry, labelled, and NULL-label paths; per-row content is covered
// above via the desc+gather+render path directly.
void test_bb_serialize_console_tasks_report_smoke(void)
{
    bb_task_base_test_reset();
    bb_serialize_console_tasks_report("tick");

    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "worker", 2048, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_set_free_bytes(&fake, 1500));
    bb_serialize_console_tasks_report("tick");
    bb_serialize_console_tasks_report(NULL);
}

// ---------------------------------------------------------------------------
// bb_format_name -- BB_FORMAT_CONSOLE coverage (BB_FORMAT_JSON/NONE/
// out-of-range already covered by test_bb_serialize_format.c).
// ---------------------------------------------------------------------------

void test_bb_format_name_console_returns_console(void)
{
    TEST_ASSERT_EQUAL_STRING("console", bb_format_name(BB_FORMAT_CONSOLE));
}
