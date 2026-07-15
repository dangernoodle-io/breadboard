// Host tests for the format-dispatch registry (bb_serialize_format.h) plus
// bb_format_name() coverage -- B1-829 PR1.
//
// Coverage targets: bb_format_name for every real enum value + out-of-range
// (both directions); register/get_emit/get_parse happy path; NULL entry;
// BB_FORMAT_NONE and out-of-range fmt rejection on register/get_*;
// nullable parse; identical re-register (idempotent no-op) and re-register
// with a different backend (rejected, not clobbered); and a
// bb_serialize_json round trip driven entirely through the registry lookups
// (no direct #include of a format's own emit/scan symbols by the consumer).

#include "unity.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"

#include <string.h>

// ---------------------------------------------------------------------------
// bb_format_name
// ---------------------------------------------------------------------------

void test_bb_format_name_none_returns_null(void)
{
    TEST_ASSERT_NULL(bb_format_name(BB_FORMAT_NONE));
}

void test_bb_format_name_json_returns_json(void)
{
    TEST_ASSERT_EQUAL_STRING("json", bb_format_name(BB_FORMAT_JSON));
}

void test_bb_format_name_count_sentinel_returns_null(void)
{
    TEST_ASSERT_NULL(bb_format_name(BB_FORMAT__COUNT));
}

void test_bb_format_name_out_of_range_negative_returns_null(void)
{
    TEST_ASSERT_NULL(bb_format_name((bb_format_t)-1));
}

void test_bb_format_name_out_of_range_positive_returns_null(void)
{
    TEST_ASSERT_NULL(bb_format_name((bb_format_t)(BB_FORMAT__COUNT + 1)));
}

// ---------------------------------------------------------------------------
// bb_serialize_format_register / get_emit / get_parse
// ---------------------------------------------------------------------------

static void dummy_begin_obj(void *ctx, const char *key) { (void)ctx; (void)key; }
static void dummy_end_obj(void *ctx) { (void)ctx; }
static void dummy_begin_arr(void *ctx, const char *key) { (void)ctx; (void)key; }
static void dummy_end_arr(void *ctx) { (void)ctx; }
static void dummy_emit_i64(void *ctx, const char *key, int64_t v) { (void)ctx; (void)key; (void)v; }
static void dummy_emit_u64(void *ctx, const char *key, uint64_t v) { (void)ctx; (void)key; (void)v; }
static void dummy_emit_f64(void *ctx, const char *key, double v) { (void)ctx; (void)key; (void)v; }
static void dummy_emit_bool(void *ctx, const char *key, bool v) { (void)ctx; (void)key; (void)v; }
static void dummy_emit_str(void *ctx, const char *key, const char *s, size_t len)
{
    (void)ctx; (void)key; (void)s; (void)len;
}
static void dummy_emit_null(void *ctx, const char *key) { (void)ctx; (void)key; }

static const bb_serialize_emit_t s_dummy_emit_a = {
    .format_id = BB_FORMAT_JSON,
    .begin_obj = dummy_begin_obj,
    .end_obj = dummy_end_obj,
    .begin_arr = dummy_begin_arr,
    .end_arr = dummy_end_arr,
    .emit_i64 = dummy_emit_i64,
    .emit_u64 = dummy_emit_u64,
    .emit_f64 = dummy_emit_f64,
    .emit_bool = dummy_emit_bool,
    .emit_str = dummy_emit_str,
    .emit_null = dummy_emit_null,
};

static const bb_serialize_emit_t s_dummy_emit_b = {
    .format_id = BB_FORMAT_JSON,
    .begin_obj = dummy_begin_obj,
    .end_obj = dummy_end_obj,
    .begin_arr = dummy_begin_arr,
    .end_arr = dummy_end_arr,
    .emit_i64 = dummy_emit_i64,
    .emit_u64 = dummy_emit_u64,
    .emit_f64 = dummy_emit_f64,
    .emit_bool = dummy_emit_bool,
    .emit_str = dummy_emit_str,
    .emit_null = dummy_emit_null,
};

static int s_dummy_parse_target = 0;

void test_bb_serialize_format_get_emit_miss_before_register(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_NULL(bb_serialize_format_get_emit(BB_FORMAT_JSON));
    TEST_ASSERT_NULL(bb_serialize_format_get_parse(BB_FORMAT_JSON));
}

void test_bb_serialize_format_register_null_entry_returns_invalid_arg(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_format_register(BB_FORMAT_JSON, NULL));
}

void test_bb_serialize_format_register_none_returns_invalid_arg(void)
{
    bb_serialize_format_test_reset();
    bb_serialize_format_entry_t entry = { .emit = &s_dummy_emit_a, .parse = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_format_register(BB_FORMAT_NONE, &entry));
}

void test_bb_serialize_format_register_out_of_range_returns_invalid_arg(void)
{
    bb_serialize_format_test_reset();
    bb_serialize_format_entry_t entry = { .emit = &s_dummy_emit_a, .parse = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_serialize_format_register((bb_format_t)(BB_FORMAT__COUNT + 1), &entry));
}

void test_bb_serialize_format_register_get_emit_roundtrip(void)
{
    bb_serialize_format_test_reset();
    bb_serialize_format_entry_t entry = { .emit = &s_dummy_emit_a, .parse = &s_dummy_parse_target };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_emit_a, bb_serialize_format_get_emit(BB_FORMAT_JSON));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_parse_target, bb_serialize_format_get_parse(BB_FORMAT_JSON));
}

void test_bb_serialize_format_get_parse_nullable(void)
{
    bb_serialize_format_test_reset();
    bb_serialize_format_entry_t entry = { .emit = &s_dummy_emit_a, .parse = NULL };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_emit_a, bb_serialize_format_get_emit(BB_FORMAT_JSON));
    TEST_ASSERT_NULL(bb_serialize_format_get_parse(BB_FORMAT_JSON));
}

// Re-register the SAME format with an entry carrying identical emit/parse
// pointers: idempotent no-op (BB_OK), the legitimate codegen-re-run case.
void test_bb_serialize_format_reregister_identical_is_noop(void)
{
    bb_serialize_format_test_reset();
    bb_serialize_format_entry_t entry_a = { .emit = &s_dummy_emit_a, .parse = &s_dummy_parse_target };
    bb_serialize_format_entry_t entry_a_again = { .emit = &s_dummy_emit_a, .parse = &s_dummy_parse_target };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry_a));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_emit_a, bb_serialize_format_get_emit(BB_FORMAT_JSON));

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry_a_again));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_emit_a, bb_serialize_format_get_emit(BB_FORMAT_JSON));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_parse_target, bb_serialize_format_get_parse(BB_FORMAT_JSON));
}

// Re-register the same format with a DIFFERENT backend (different
// emit/parse pointers): rejected, not clobbered -- the prior entry survives.
void test_bb_serialize_format_reregister_different_backend_rejected(void)
{
    bb_serialize_format_test_reset();
    bb_serialize_format_entry_t entry_a = { .emit = &s_dummy_emit_a, .parse = NULL };
    bb_serialize_format_entry_t entry_b = { .emit = &s_dummy_emit_b, .parse = &s_dummy_parse_target };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry_a));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_emit_a, bb_serialize_format_get_emit(BB_FORMAT_JSON));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_serialize_format_register(BB_FORMAT_JSON, &entry_b));
    // Prior entry is untouched -- no clobber.
    TEST_ASSERT_EQUAL_PTR(&s_dummy_emit_a, bb_serialize_format_get_emit(BB_FORMAT_JSON));
    TEST_ASSERT_NULL(bb_serialize_format_get_parse(BB_FORMAT_JSON));
}

// Re-register the same format with an entry whose `emit` pointer MATCHES
// the existing entry but whose `parse` pointer DIFFERS: still rejected (the
// identical-entry check requires both to match) -- and, like the
// both-differ case above, the prior entry survives untouched.
static int s_dummy_parse_target_2 = 0;

void test_bb_serialize_format_reregister_emit_matches_parse_differs_rejected(void)
{
    bb_serialize_format_test_reset();
    bb_serialize_format_entry_t entry_a = { .emit = &s_dummy_emit_a, .parse = &s_dummy_parse_target };
    bb_serialize_format_entry_t entry_c = { .emit = &s_dummy_emit_a, .parse = &s_dummy_parse_target_2 };

    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_format_register(BB_FORMAT_JSON, &entry_a));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_emit_a, bb_serialize_format_get_emit(BB_FORMAT_JSON));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_parse_target, bb_serialize_format_get_parse(BB_FORMAT_JSON));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_serialize_format_register(BB_FORMAT_JSON, &entry_c));
    // Prior entry is untouched -- no clobber.
    TEST_ASSERT_EQUAL_PTR(&s_dummy_emit_a, bb_serialize_format_get_emit(BB_FORMAT_JSON));
    TEST_ASSERT_EQUAL_PTR(&s_dummy_parse_target, bb_serialize_format_get_parse(BB_FORMAT_JSON));
}

// get_emit/get_parse called with an fmt that can never be registered
// (BB_FORMAT_NONE or out-of-range) hit the lookup helper's `!name` branch
// directly, independent of whatever is or isn't currently registered.
void test_bb_serialize_format_get_emit_none_returns_null(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_NULL(bb_serialize_format_get_emit(BB_FORMAT_NONE));
}

void test_bb_serialize_format_get_emit_out_of_range_returns_null(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_NULL(bb_serialize_format_get_emit((bb_format_t)(BB_FORMAT__COUNT + 1)));
}

void test_bb_serialize_format_get_parse_none_returns_null(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_NULL(bb_serialize_format_get_parse(BB_FORMAT_NONE));
}

void test_bb_serialize_format_get_parse_out_of_range_returns_null(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_NULL(bb_serialize_format_get_parse((bb_format_t)(BB_FORMAT__COUNT + 1)));
}

// ---------------------------------------------------------------------------
// bb_serialize_json round trip through the registry -- the consumer never
// #includes a format-specific emit/scan symbol directly, only looks it up
// by bb_format_t.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} rt_snap_t;

static const bb_serialize_field_t s_rt_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(rt_snap_t, n) },
};

static const bb_serialize_desc_t s_rt_desc = {
    .type_name = "rt_snap_t",
    .fields = s_rt_fields,
    .n_fields = 1,
    .snap_size = sizeof(rt_snap_t),
};

void test_bb_serialize_format_json_render_roundtrip_via_registry(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_register_format());

    const bb_serialize_emit_t *tmpl = bb_serialize_format_get_emit(BB_FORMAT_JSON);
    TEST_ASSERT_NOT_NULL(tmpl);
    TEST_ASSERT_EQUAL(BB_FORMAT_JSON, tmpl->format_id);

    // The registered vtable is a ctx-less template -- copy it and bind a
    // fresh writer ctx before walking, exactly as a future consumer
    // (bb_cache_serialize, PR2) would.
    bb_serialize_json_ctx_t writer;
    char buf[64];
    bb_serialize_json_ctx_init(&writer, buf, sizeof(buf) - 1);

    bb_serialize_emit_t emit = *tmpl;
    emit.ctx = &writer;

    rt_snap_t snap = { .n = 42 };

    // Mirror bb_serialize_json_render()'s own '{'...'}' wrapping since we're
    // driving the walker manually via the looked-up vtable -- the writer's
    // root level (stack[0]) is already object-context from ctx_init(), so
    // wrapping is a raw brace write, not another begin_obj/end_obj pair
    // (that would push a second, key-requiring level).
    buf[0] = '{';
    writer.len = 1;
    bb_serialize_walk(&s_rt_desc, &snap, &emit);
    TEST_ASSERT_EQUAL(BB_OK, writer.err);
    buf[writer.len++] = '}';
    buf[writer.len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"n\":42}", buf);

    // parse side: cast the opaque handle back to bb_serialize_json's own
    // scan_bounded signature and confirm it can scan the JSON we just
    // rendered.
    typedef bb_err_t (*scan_bounded_fn)(const char *, size_t, const bb_serialize_json_ingest_t *);
    scan_bounded_fn scan = (scan_bounded_fn)bb_serialize_format_get_parse(BB_FORMAT_JSON);
    TEST_ASSERT_NOT_NULL(scan);

    bb_serialize_json_tok_t pool[8];
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_serialize_json_tok_recorder_init(&rec, buf, strlen(buf), pool, 8, NULL, 0));
    bb_serialize_json_ingest_t sink = bb_serialize_json_tok_recorder_ingest(&rec);

    TEST_ASSERT_EQUAL(BB_OK, scan(buf, strlen(buf), &sink));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    TEST_ASSERT_TRUE(bb_serialize_json_tok_is_obj(&rec, root));
    bb_serialize_json_tok_idx_t n_tok = bb_serialize_json_tok_obj_get(&rec, root, "n", 1);
    int64_t n_val = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, n_tok, &n_val));
    TEST_ASSERT_EQUAL_INT64(42, n_val);
}

// bb_serialize_json_register_format() is idempotent -- calling it a second
// time (e.g. codegen wiring it in two firmwares' test builds) still
// succeeds and leaves exactly one live registration (last-writer-wins).
void test_bb_serialize_json_register_format_idempotent(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_register_format());
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_register_format());
    TEST_ASSERT_NOT_NULL(bb_serialize_format_get_emit(BB_FORMAT_JSON));
}
