// Host tests for the bb_diag section registry (B1-diag-dissolution PR3):
// bb_diag_register_section() plus the pure, portable dispatch helpers
// (bb_diag_section_find/name_from_uri/build_query) that the ESP-IDF
// dispatcher (platform/espidf/bb_diag/bb_diag_section_dispatch.c) drives at
// request time. This file reconstructs that SAME sequence end to end
// (name_from_uri -> find -> build_query -> fill -> render) against a fake
// producer -- the one-tested-path seam the dispatcher's own code just
// glues httpd types onto. Mirrors test_bb_data.c's fixture idiom.

#include "unity.h"

#include "bb_diag_section_priv.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixture: a tiny snapshot type + fake format entry + fake query getter.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} ds_snap_t;

static const bb_serialize_field_t s_ds_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(ds_snap_t, n) },
};

static const bb_serialize_desc_t s_ds_desc = {
    .type_name = "ds_snap_t",
    .fields    = s_ds_fields,
    .n_fields  = 1,
    .snap_size = sizeof(ds_snap_t),
};

// A snap_desc whose snap_size deliberately exceeds
// BB_DIAG_SECTION_SCRATCH_BYTES (512 by default, no Kconfig override in the
// host build) -- used only to exercise bb_diag_register_section()'s loud
// attach-time reject; never actually filled/rendered.
typedef struct {
    char big[BB_DIAG_SECTION_SCRATCH_BYTES + 1];
} ds_oversize_snap_t;

static const bb_serialize_desc_t s_ds_oversize_desc = {
    .type_name = "ds_oversize_snap_t",
    .fields    = NULL,
    .n_fields  = 0,
    .snap_size = sizeof(ds_oversize_snap_t),
};

static bb_err_t ds_fill_ok(void *dst, const bb_diag_fill_args_t *args)
{
    ((ds_snap_t *)dst)->n = *(int64_t *)args->ctx;
    return BB_OK;
}

static bb_err_t ds_fill_fail(void *dst, const bb_diag_fill_args_t *args)
{
    (void)dst;
    (void)args;
    return BB_ERR_INVALID_STATE;
}

// Asserts the request-scoped query carried through to the fill hook matches
// the expected "type" value, then fills like ds_fill_ok().
static const char *s_ds_fill_query_expected_type = NULL;

static bb_err_t ds_fill_query(void *dst, const bb_diag_fill_args_t *args)
{
    TEST_ASSERT_EQUAL_STRING(s_ds_fill_query_expected_type,
                              bb_serialize_query_get(args->query, "type"));
    ((ds_snap_t *)dst)->n = *(int64_t *)args->ctx;
    return BB_OK;
}

static void ds_register_format(void)
{
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_register_format());
}

static void ds_reset(void)
{
    bb_diag_section_test_reset();
}

// Fake query getter -- looks `key` up in a caller-supplied array of
// key/value pairs (ctx). Returns false (absent) for any key not present.
typedef struct {
    const char *key;
    const char *value;
} ds_kv_t;

typedef struct {
    const ds_kv_t *kvs;
    size_t         n;
} ds_kv_fixture_t;

static bool ds_kv_getter(void *ctx, const char *key, char *out, size_t out_cap)
{
    const ds_kv_fixture_t *fx = (const ds_kv_fixture_t *)ctx;
    for (size_t i = 0; i < fx->n; i++) {
        if (strcmp(fx->kvs[i].key, key) == 0) {
            strncpy(out, fx->kvs[i].value, out_cap - 1);
            out[out_cap - 1] = '\0';
            return true;
        }
    }
    return false;
}

static const char *const s_type_query_keys[] = { "type" };

// ---------------------------------------------------------------------------
// bb_diag_register_section
// ---------------------------------------------------------------------------

void test_bb_diag_register_section_success(void)
{
    ds_reset();
    int64_t ctx_val = 1;
    bb_diag_section_t sec = { .name = "ds.ok", .desc = "ok", .snap_desc = &s_ds_desc,
                              .fill = ds_fill_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_null_section_returns_invalid_arg(void)
{
    ds_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(NULL));
}

void test_bb_diag_register_section_null_name_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = NULL, .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_null_snap_desc_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = "ds.nodesc", .snap_desc = NULL, .fill = ds_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_null_fill_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = "ds.nofill", .snap_desc = &s_ds_desc, .fill = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_name_too_long_returns_invalid_arg(void)
{
    ds_reset();
    char name_over[BB_DIAG_SECTION_NAME_MAX + 1];
    memset(name_over, 'n', sizeof(name_over) - 1);
    name_over[sizeof(name_over) - 1] = '\0';  // strlen == BB_DIAG_SECTION_NAME_MAX, over

    bb_diag_section_t sec = { .name = name_over, .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_name_max_boundary_ok(void)
{
    ds_reset();
    char name_ok[BB_DIAG_SECTION_NAME_MAX];
    memset(name_ok, 'n', sizeof(name_ok) - 1);
    name_ok[sizeof(name_ok) - 1] = '\0';  // strlen == BB_DIAG_SECTION_NAME_MAX - 1, fits

    bb_diag_section_t sec = { .name = name_ok, .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_too_many_query_keys_returns_invalid_arg(void)
{
    ds_reset();
    static const char *const too_many[] = { "a", "b", "c", "d", "e" };  // 5 > BB_SERIALIZE_QUERY_MAX_PARAMS (4)
    bb_diag_section_t sec = {
        .name = "ds.toomanyqk", .snap_desc = &s_ds_desc, .fill = ds_fill_ok,
        .query_keys = too_many, .n_query_keys = 5,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_null_query_keys_with_nonzero_count_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.nullqk", .snap_desc = &s_ds_desc, .fill = ds_fill_ok,
        .query_keys = NULL, .n_query_keys = 1,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_duplicate_name_returns_invalid_state(void)
{
    ds_reset();
    bb_diag_section_t a = { .name = "ds.dup", .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
    bb_diag_section_t b = { .name = "ds.dup", .snap_desc = &s_ds_desc, .fill = ds_fill_fail };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&a));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_diag_register_section(&b));

    // First-wins: the original registration is untouched.
    const bb_diag_section_t *found = bb_diag_section_find("ds.dup");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_PTR(ds_fill_ok, found->fill);
}

void test_bb_diag_register_section_table_full_returns_no_space(void)
{
    ds_reset();
    char names[BB_DIAG_SECTION_TABLE_CAP + 1][BB_DIAG_SECTION_NAME_MAX];
    for (int i = 0; i < BB_DIAG_SECTION_TABLE_CAP; i++) {
        snprintf(names[i], sizeof(names[i]), "ds.cap.%d", i);
        bb_diag_section_t sec = { .name = names[i], .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
        TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));
    }

    snprintf(names[BB_DIAG_SECTION_TABLE_CAP], sizeof(names[BB_DIAG_SECTION_TABLE_CAP]),
             "ds.cap.%d", BB_DIAG_SECTION_TABLE_CAP);
    bb_diag_section_t overflow = {
        .name = names[BB_DIAG_SECTION_TABLE_CAP], .snap_desc = &s_ds_desc, .fill = ds_fill_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_diag_register_section(&overflow));
}

void test_bb_diag_register_section_duplicate_name_wins_over_table_full(void)
{
    ds_reset();
    char names[BB_DIAG_SECTION_TABLE_CAP][BB_DIAG_SECTION_NAME_MAX];
    for (int i = 0; i < BB_DIAG_SECTION_TABLE_CAP; i++) {
        snprintf(names[i], sizeof(names[i]), "ds.dupcap.%d", i);
        bb_diag_section_t sec = { .name = names[i], .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
        TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));
    }

    // Table is now full; re-registering an ALREADY-bound name still reports
    // the duplicate as BB_ERR_INVALID_STATE, not BB_ERR_NO_SPACE.
    bb_diag_section_t redup = { .name = names[0], .snap_desc = &s_ds_desc, .fill = ds_fill_fail };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_diag_register_section(&redup));
}

void test_bb_diag_register_section_snap_size_exceeds_scratch_returns_no_space(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = "ds.oversize", .snap_desc = &s_ds_oversize_desc, .fill = ds_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_diag_register_section(&sec));
}

// ---------------------------------------------------------------------------
// bb_diag_section_name_from_uri
// ---------------------------------------------------------------------------

void test_bb_diag_section_name_from_uri_extracts_name(void)
{
    char out[BB_DIAG_SECTION_NAME_MAX];
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_name_from_uri("/api/diag/meminfo", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("meminfo", out);
}

void test_bb_diag_section_name_from_uri_wrong_prefix_returns_not_found(void)
{
    char out[BB_DIAG_SECTION_NAME_MAX];
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_diag_section_name_from_uri("/api/other/x", out, sizeof(out)));
}

void test_bb_diag_section_name_from_uri_truncates_to_out_cap(void)
{
    char out[4];
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_name_from_uri("/api/diag/meminfo", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mem", out);
}

void test_bb_diag_section_name_from_uri_null_args_return_invalid_arg(void)
{
    char out[BB_DIAG_SECTION_NAME_MAX];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_section_name_from_uri(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_section_name_from_uri("/api/diag/x", NULL, sizeof(out)));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_section_name_from_uri("/api/diag/x", out, 0));
}

// ---------------------------------------------------------------------------
// bb_diag_section_find
// ---------------------------------------------------------------------------

void test_bb_diag_section_find_null_name_returns_null(void)
{
    ds_reset();
    TEST_ASSERT_NULL(bb_diag_section_find(NULL));
}

void test_bb_diag_section_find_unregistered_returns_null(void)
{
    ds_reset();
    TEST_ASSERT_NULL(bb_diag_section_find("ds.nope"));
}

// ---------------------------------------------------------------------------
// bb_diag_section_build_query
// ---------------------------------------------------------------------------

void test_bb_diag_section_build_query_no_keys_sets_count_zero(void)
{
    bb_diag_section_t sec = { .name = "ds.noqk", .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
    char value_scratch[4][BB_DIAG_SECTION_QUERY_VALUE_BYTES];
    bb_serialize_query_t query = { .count = 99 };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_build_query(&sec, ds_kv_getter, NULL, (char *)value_scratch, &query));
    TEST_ASSERT_EQUAL_UINT(0, query.count);
}

void test_bb_diag_section_build_query_threads_found_value(void)
{
    bb_diag_section_t sec = {
        .name = "ds.qk", .snap_desc = &s_ds_desc, .fill = ds_fill_ok,
        .query_keys = s_type_query_keys, .n_query_keys = 1,
    };
    ds_kv_t kvs[] = { { "type", "raw" } };
    ds_kv_fixture_t fx = { .kvs = kvs, .n = 1 };
    char value_scratch[4][BB_DIAG_SECTION_QUERY_VALUE_BYTES];
    bb_serialize_query_t query = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_build_query(&sec, ds_kv_getter, &fx, (char *)value_scratch, &query));
    TEST_ASSERT_EQUAL_UINT(1, query.count);
    TEST_ASSERT_EQUAL_STRING("raw", bb_serialize_query_get(&query, "type"));
}

void test_bb_diag_section_build_query_missing_key_omitted(void)
{
    bb_diag_section_t sec = {
        .name = "ds.qkmiss", .snap_desc = &s_ds_desc, .fill = ds_fill_ok,
        .query_keys = s_type_query_keys, .n_query_keys = 1,
    };
    ds_kv_fixture_t fx = { .kvs = NULL, .n = 0 };
    char value_scratch[4][BB_DIAG_SECTION_QUERY_VALUE_BYTES];
    bb_serialize_query_t query = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_build_query(&sec, ds_kv_getter, &fx, (char *)value_scratch, &query));
    TEST_ASSERT_EQUAL_UINT(0, query.count);
}

void test_bb_diag_section_build_query_null_args_return_invalid_arg(void)
{
    bb_diag_section_t sec = { .name = "ds.qknull", .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
    char value_scratch[4][BB_DIAG_SECTION_QUERY_VALUE_BYTES];
    bb_serialize_query_t query = {0};

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_diag_section_build_query(NULL, ds_kv_getter, NULL, (char *)value_scratch, &query));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_diag_section_build_query(&sec, NULL, NULL, (char *)value_scratch, &query));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_diag_section_build_query(&sec, ds_kv_getter, NULL, NULL, &query));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_diag_section_build_query(&sec, ds_kv_getter, NULL, (char *)value_scratch, NULL));
}

// ---------------------------------------------------------------------------
// Full dispatch sequence, driven entirely through the pure helpers -- the
// SAME sequence platform/espidf/bb_diag/bb_diag_section_dispatch.c's
// diag_section_dispatch() drives, minus the httpd glue. Proves the one
// tested code path both the device handler and this test exercise.
// ---------------------------------------------------------------------------

void test_bb_diag_dispatch_hit_renders_json(void)
{
    ds_reset();
    ds_register_format();

    int64_t ctx_val = 7;
    bb_diag_section_t sec = { .name = "ds.hit", .snap_desc = &s_ds_desc, .fill = ds_fill_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));

    char name[BB_DIAG_SECTION_NAME_MAX];
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_name_from_uri("/api/diag/ds.hit", name, sizeof(name)));

    const bb_diag_section_t *found = bb_diag_section_find(name);
    TEST_ASSERT_NOT_NULL(found);

    char value_scratch[4][BB_DIAG_SECTION_QUERY_VALUE_BYTES];
    bb_serialize_query_t query = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_build_query(found, ds_kv_getter, NULL, (char *)value_scratch, &query));

    char scratch[sizeof(ds_snap_t)];
    bb_diag_fill_args_t fill_args = { .ctx = found->ctx, .query = found->n_query_keys > 0 ? &query : NULL };
    TEST_ASSERT_EQUAL(BB_OK, found->fill(scratch, &fill_args));

    char   buf[64];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_serialize_format_render(BB_FORMAT_JSON, found->snap_desc, scratch, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"n\":7}", buf);
}

void test_bb_diag_dispatch_miss_wrong_prefix_returns_not_found(void)
{
    ds_reset();
    char name[BB_DIAG_SECTION_NAME_MAX];
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_diag_section_name_from_uri("/api/other/x", name, sizeof(name)));
}

void test_bb_diag_dispatch_miss_unregistered_name_returns_null(void)
{
    ds_reset();
    char name[BB_DIAG_SECTION_NAME_MAX];
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_name_from_uri("/api/diag/nope", name, sizeof(name)));
    TEST_ASSERT_NULL(bb_diag_section_find(name));
}

void test_bb_diag_dispatch_query_threading(void)
{
    ds_reset();
    ds_register_format();

    int64_t ctx_val = 3;
    bb_diag_section_t sec = {
        .name = "ds.qthread", .snap_desc = &s_ds_desc, .fill = ds_fill_query, .ctx = &ctx_val,
        .query_keys = s_type_query_keys, .n_query_keys = 1,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));

    const bb_diag_section_t *found = bb_diag_section_find("ds.qthread");
    TEST_ASSERT_NOT_NULL(found);

    ds_kv_t kvs[] = { { "type", "raw" } };
    ds_kv_fixture_t fx = { .kvs = kvs, .n = 1 };
    char value_scratch[4][BB_DIAG_SECTION_QUERY_VALUE_BYTES];
    bb_serialize_query_t query = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_section_build_query(found, ds_kv_getter, &fx, (char *)value_scratch, &query));

    s_ds_fill_query_expected_type = "raw";
    char scratch[sizeof(ds_snap_t)];
    bb_diag_fill_args_t fill_args = { .ctx = found->ctx, .query = &query };
    TEST_ASSERT_EQUAL(BB_OK, found->fill(scratch, &fill_args));
    TEST_ASSERT_EQUAL_INT64(3, ((ds_snap_t *)scratch)->n);
}

void test_bb_diag_dispatch_fill_failure_propagates(void)
{
    ds_reset();
    ds_register_format();

    bb_diag_section_t sec = { .name = "ds.fillfail", .snap_desc = &s_ds_desc, .fill = ds_fill_fail };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));

    const bb_diag_section_t *found = bb_diag_section_find("ds.fillfail");
    TEST_ASSERT_NOT_NULL(found);

    char scratch[sizeof(ds_snap_t)];
    bb_diag_fill_args_t fill_args = { .ctx = found->ctx, .query = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, found->fill(scratch, &fill_args));
}

// ---------------------------------------------------------------------------
// iter/fill XOR + stream-field reg-time validation (B1-1077 PR-2)
// ---------------------------------------------------------------------------

typedef struct {
    int64_t v;
} ds_row_t;

static const bb_serialize_field_t s_ds_row_fields[] = {
    { .key = "v", .type = BB_TYPE_I64, .offset = offsetof(ds_row_t, v) },
};

typedef struct {
    bb_serialize_arr_stream_t   items;
    bb_serialize_arr_buf_iter_t items_state;
} ds_stream_snap_t;

static const bb_serialize_field_t s_ds_stream_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(ds_stream_snap_t, items),
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_row_t),
      .children = s_ds_row_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_ds_stream_desc = {
    .type_name = "ds_stream_snap_t",
    .fields    = s_ds_stream_fields,
    .n_fields  = 1,
    .snap_size = sizeof(ds_stream_snap_t),
};

// Two stream fields at top level -- reg-time-invalid (exactly one required).
static const bb_serialize_field_t s_ds_two_stream_fields[] = {
    { .key = "a", .type = BB_TYPE_ARR, .offset = 0,
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_row_t),
      .children = s_ds_row_fields, .n_children = 1 },
    { .key = "b", .type = BB_TYPE_ARR, .offset = sizeof(bb_serialize_arr_stream_t),
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_row_t),
      .children = s_ds_row_fields, .n_children = 1 },
};
static const bb_serialize_desc_t s_ds_two_stream_desc = {
    .type_name = "ds_two_stream_t",
    .fields    = s_ds_two_stream_fields,
    .n_fields  = 2,
    .snap_size = 2 * sizeof(bb_serialize_arr_stream_t),
};

// A STREAM field whose elem_type is BB_TYPE_STR, not BB_TYPE_OBJ --
// reg-time-invalid (an iter section's row shape must be OBJ).
static const bb_serialize_field_t s_ds_stream_str_elem_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = 0,
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_STR, .max_len = 8 },
};
static const bb_serialize_desc_t s_ds_stream_str_elem_desc = {
    .type_name = "ds_stream_str_elem_t",
    .fields    = s_ds_stream_str_elem_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bb_serialize_arr_stream_t),
};

// A STREAM field whose elem_size exceeds BB_SERIALIZE_MAX_ROW_BYTES.
typedef struct {
    char big[BB_SERIALIZE_MAX_ROW_BYTES + 1];
} ds_oversize_row_t;
static const bb_serialize_field_t s_ds_oversize_row_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = 0,
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_oversize_row_t) },
};
static const bb_serialize_desc_t s_ds_oversize_row_desc = {
    .type_name = "ds_oversize_row_t",
    .fields    = s_ds_oversize_row_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bb_serialize_arr_stream_t),
};

// A STREAM field whose elem_size is 0 -- an authoring mistake (forgot to
// set `.elem_size`), distinct from the oversize case above. Left
// unvalidated, this would let a 0-byte-per-row arena allocation
// (bb_malloc_prefer_spiram(row_count * 0)) through the dispatcher's
// overflow guard (which only fires for a NONZERO elem_size), then have
// phase-2 fill write real elem_size-sized rows past a 0-byte block.
static const bb_serialize_field_t s_ds_zero_elem_size_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = 0,
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = 0,
      .children = s_ds_row_fields, .n_children = 1 },
};
static const bb_serialize_desc_t s_ds_zero_elem_size_desc = {
    .type_name = "ds_zero_elem_size_t",
    .fields    = s_ds_zero_elem_size_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bb_serialize_arr_stream_t),
};

static bb_err_t ds_iter_ok(void *dst, void *row_arena, size_t row_cap, size_t *row_count,
                            const bb_diag_fill_args_t *args)
{
    (void)dst; (void)row_arena; (void)row_cap; (void)args;
    *row_count = 0;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Nested BB_ARR_STREAM fields (below the top level) -- reg-time-invalid.
// The iter/dispatcher model wires the carrier ONLY at a top-level field's
// own offset; a STREAM field nested inside an OBJ child, or inside a
// (FIXED) ARR-of-OBJ element's row shape, has no such wiring point and
// would bypass the top-level elem_size <= BB_SERIALIZE_MAX_ROW_BYTES check.
// ---------------------------------------------------------------------------

// Case 1: a top-level OBJ field ("mid") whose child is ITSELF an OBJ
// ("child"), whose OWN children table nests the STREAM field -- three
// levels deep, so reject_nested_stream_fields() must recurse through an
// intermediate OBJ level (depth 1 -> 2) that has no stream field of its
// own before finding the violation at depth 2, exercising the recursive
// call itself (not just an immediate depth-1 match).
typedef struct {
    bb_serialize_arr_stream_t   nested_items;
    bb_serialize_arr_buf_iter_t nested_items_state;
} ds_obj_child_t;

typedef struct {
    ds_obj_child_t child;
} ds_obj_mid_t;

typedef struct {
    ds_obj_mid_t mid;
} ds_nested_stream_in_obj_snap_t;

static const bb_serialize_field_t s_ds_obj_child_fields[] = {
    { .key = "nested_items", .type = BB_TYPE_ARR, .offset = offsetof(ds_obj_child_t, nested_items),
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_row_t),
      .children = s_ds_row_fields, .n_children = 1 },
};

static const bb_serialize_field_t s_ds_obj_mid_fields[] = {
    { .key = "child", .type = BB_TYPE_OBJ, .offset = offsetof(ds_obj_mid_t, child),
      .children = s_ds_obj_child_fields, .n_children = 1 },
};

static const bb_serialize_field_t s_ds_nested_stream_in_obj_fields[] = {
    { .key = "mid", .type = BB_TYPE_OBJ, .offset = offsetof(ds_nested_stream_in_obj_snap_t, mid),
      .children = s_ds_obj_mid_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_ds_nested_stream_in_obj_desc = {
    .type_name = "ds_nested_stream_in_obj_snap_t",
    .fields    = s_ds_nested_stream_in_obj_fields,
    .n_fields  = 1,
    .snap_size = sizeof(ds_nested_stream_in_obj_snap_t),
};

// Case 2: a top-level FIXED (cardinality == 0) ARR-of-OBJ field whose
// element row shape nests a STREAM field.
typedef struct {
    ds_obj_child_t inner;  // reuses the same "nested_items" STREAM shape as case 1
} ds_arr_elem_with_stream_t;

typedef struct {
    bb_serialize_arr_t rows;  // FIXED array of ds_arr_elem_with_stream_t
} ds_nested_stream_in_arr_of_obj_snap_t;

static const bb_serialize_field_t s_ds_arr_elem_with_stream_fields[] = {
    { .key = "nested_items", .type = BB_TYPE_ARR, .offset = offsetof(ds_arr_elem_with_stream_t, inner.nested_items),
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_row_t),
      .children = s_ds_row_fields, .n_children = 1 },
};

static const bb_serialize_field_t s_ds_nested_stream_in_arr_of_obj_fields[] = {
    { .key = "rows", .type = BB_TYPE_ARR, .offset = offsetof(ds_nested_stream_in_arr_of_obj_snap_t, rows),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_arr_elem_with_stream_t), .max_items = 4,
      .children = s_ds_arr_elem_with_stream_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_ds_nested_stream_in_arr_of_obj_desc = {
    .type_name = "ds_nested_stream_in_arr_of_obj_snap_t",
    .fields    = s_ds_nested_stream_in_arr_of_obj_fields,
    .n_fields  = 1,
    .snap_size = sizeof(ds_nested_stream_in_arr_of_obj_snap_t),
};

// Case 3: same ARR-of-OBJ-nesting-a-STREAM row shape as case 2 above, but
// wrapped one level deeper under a top-level OBJ field -- unlike case 2
// (where validate_stream_field()'s OWN top-level loop finds the ARR-of-OBJ
// field directly), this forces reject_nested_stream_fields() itself (not
// just validate_stream_field()) to recurse through an ARR-of-OBJ child
// (rather than only an OBJ child, as case 1 exercises) before finding the
// violation.
typedef struct {
    bb_serialize_arr_t rows;  // FIXED array of ds_arr_elem_with_stream_t, one OBJ level down
} ds_mid_with_arr_of_obj_t;

typedef struct {
    ds_mid_with_arr_of_obj_t mid;
} ds_nested_stream_via_mid_arr_of_obj_snap_t;

static const bb_serialize_field_t s_ds_mid_arr_of_obj_fields[] = {
    { .key = "rows", .type = BB_TYPE_ARR, .offset = offsetof(ds_mid_with_arr_of_obj_t, rows),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_arr_elem_with_stream_t), .max_items = 4,
      .children = s_ds_arr_elem_with_stream_fields, .n_children = 1 },
};

static const bb_serialize_field_t s_ds_nested_stream_via_mid_arr_of_obj_fields[] = {
    { .key = "mid", .type = BB_TYPE_OBJ, .offset = offsetof(ds_nested_stream_via_mid_arr_of_obj_snap_t, mid),
      .children = s_ds_mid_arr_of_obj_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_ds_nested_stream_via_mid_arr_of_obj_desc = {
    .type_name = "ds_nested_stream_via_mid_arr_of_obj_snap_t",
    .fields    = s_ds_nested_stream_via_mid_arr_of_obj_fields,
    .n_fields  = 1,
    .snap_size = sizeof(ds_nested_stream_via_mid_arr_of_obj_snap_t),
};

// A clean top-level OBJ sibling (no STREAM field anywhere below it) next to
// a valid top-level STREAM field -- reject_nested_stream_fields() recurses
// into "clean"'s children and returns BB_OK with no violation, so the loop
// in validate_stream_field() must keep going (rather than short-circuit)
// and the section still registers successfully.
typedef struct {
    int64_t x;
} ds_clean_leaf_t;

static const bb_serialize_field_t s_ds_clean_leaf_fields[] = {
    { .key = "x", .type = BB_TYPE_I64, .offset = offsetof(ds_clean_leaf_t, x) },
};

typedef struct {
    ds_clean_leaf_t leaf;
    int64_t         counts[4];  // FIXED scalar-elem ARR sibling -- see s_ds_clean_obj_fields below
} ds_clean_obj_t;

// "counts" is a nested (depth >= 1) FIXED ARR field whose elem_type is a
// scalar, not BB_TYPE_OBJ -- alongside "leaf" (an OBJ child), this makes
// reject_nested_stream_fields()'s own children-table check (not just
// validate_stream_field()'s top-level one) see a field that takes neither
// the OBJ nor the ARR-of-OBJ arm.
static const bb_serialize_field_t s_ds_clean_obj_fields[] = {
    { .key = "leaf", .type = BB_TYPE_OBJ, .offset = offsetof(ds_clean_obj_t, leaf),
      .children = s_ds_clean_leaf_fields, .n_children = 1 },
    { .key = "counts", .type = BB_TYPE_ARR, .offset = offsetof(ds_clean_obj_t, counts),
      .elem_type = BB_TYPE_I64, .max_items = 4 },
};

typedef struct {
    bb_serialize_arr_stream_t   items;
    bb_serialize_arr_buf_iter_t items_state;
    ds_clean_obj_t              clean;
} ds_stream_with_clean_obj_snap_t;

static const bb_serialize_field_t s_ds_stream_with_clean_obj_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(ds_stream_with_clean_obj_snap_t, items),
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_row_t),
      .children = s_ds_row_fields, .n_children = 1 },
    { .key = "clean", .type = BB_TYPE_OBJ, .offset = offsetof(ds_stream_with_clean_obj_snap_t, clean),
      .children = s_ds_clean_obj_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_ds_stream_with_clean_obj_desc = {
    .type_name = "ds_stream_with_clean_obj_snap_t",
    .fields    = s_ds_stream_with_clean_obj_fields,
    .n_fields  = 2,
    .snap_size = sizeof(ds_stream_with_clean_obj_snap_t),
};

// A top-level FIXED (non-STREAM) ARR field whose elem_type is a scalar, not
// BB_TYPE_OBJ -- next to a valid top-level STREAM field. validate_stream_field()
// checks every top-level field (not just STREAM ones) against "does this
// carry a children table", and a scalar-elem ARR field is the one shape that
// takes neither the OBJ arm nor the ARR-of-OBJ arm of that check.
typedef struct {
    bb_serialize_arr_stream_t   items;
    bb_serialize_arr_buf_iter_t items_state;
    int64_t                     counts[4];
} ds_stream_with_scalar_arr_snap_t;

static const bb_serialize_field_t s_ds_stream_with_scalar_arr_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(ds_stream_with_scalar_arr_snap_t, items),
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_row_t),
      .children = s_ds_row_fields, .n_children = 1 },
    { .key = "counts", .type = BB_TYPE_ARR, .offset = offsetof(ds_stream_with_scalar_arr_snap_t, counts),
      .elem_type = BB_TYPE_I64, .max_items = 4 },
};

static const bb_serialize_desc_t s_ds_stream_with_scalar_arr_desc = {
    .type_name = "ds_stream_with_scalar_arr_snap_t",
    .fields    = s_ds_stream_with_scalar_arr_fields,
    .n_fields  = 2,
    .snap_size = sizeof(ds_stream_with_scalar_arr_snap_t),
};

// A STREAM field nested 8 OBJ levels below the top ("mid" -> 7 more "next"
// hops) -- at or past BB_SERIALIZE_MAX_DEPTH (8), reject_nested_stream_fields()
// fail-opens (returns BB_OK without scanning) rather than rejecting, mirroring
// the walker's own recursion guard: a field that deep is unreachable by the
// walker anyway, so validating it is pointless. Registration (as a `fill`
// section -- no top-level STREAM field exists here to also engage the
// separate iter-contract checks) succeeds despite the buried STREAM field.
static const bb_serialize_field_t s_ds_depth_l8_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = 0,
      .cardinality = BB_ARR_STREAM, .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(ds_row_t),
      .children = s_ds_row_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_ds_depth_l7_fields[] = {
    { .key = "next", .type = BB_TYPE_OBJ, .offset = 0, .children = s_ds_depth_l8_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_ds_depth_l6_fields[] = {
    { .key = "next", .type = BB_TYPE_OBJ, .offset = 0, .children = s_ds_depth_l7_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_ds_depth_l5_fields[] = {
    { .key = "next", .type = BB_TYPE_OBJ, .offset = 0, .children = s_ds_depth_l6_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_ds_depth_l4_fields[] = {
    { .key = "next", .type = BB_TYPE_OBJ, .offset = 0, .children = s_ds_depth_l5_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_ds_depth_l3_fields[] = {
    { .key = "next", .type = BB_TYPE_OBJ, .offset = 0, .children = s_ds_depth_l4_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_ds_depth_l2_fields[] = {
    { .key = "next", .type = BB_TYPE_OBJ, .offset = 0, .children = s_ds_depth_l3_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_ds_depth_l1_fields[] = {
    { .key = "next", .type = BB_TYPE_OBJ, .offset = 0, .children = s_ds_depth_l2_fields, .n_children = 1 },
};
static const bb_serialize_field_t s_ds_depth_cap_fields[] = {
    { .key = "mid", .type = BB_TYPE_OBJ, .offset = 0, .children = s_ds_depth_l1_fields, .n_children = 1 },
};
static const bb_serialize_desc_t s_ds_depth_cap_desc = {
    .type_name = "ds_depth_cap_t",
    .fields    = s_ds_depth_cap_fields,
    .n_fields  = 1,
    .snap_size = 8,  // never actually filled/walked by this test
};

void test_bb_diag_register_section_both_fill_and_iter_set_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.bothset", .snap_desc = &s_ds_desc, .fill = ds_fill_ok, .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_iter_success(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = "ds.iterok", .snap_desc = &s_ds_stream_desc, .iter = ds_iter_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));

    const bb_diag_section_t *found = bb_diag_section_find("ds.iterok");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_UINT(sizeof(ds_row_t), bb_diag_section_stream_elem_size(found));
}

void test_bb_diag_register_section_fill_section_stream_elem_size_is_zero(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = "ds.fillzero", .snap_desc = &s_ds_desc, .fill = ds_fill_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));

    const bb_diag_section_t *found = bb_diag_section_find("ds.fillzero");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_UINT(0, bb_diag_section_stream_elem_size(found));
}

void test_bb_diag_section_stream_elem_size_null_sec_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(0, bb_diag_section_stream_elem_size(NULL));
}

// A stray/synthetic bb_diag_section_t* far outside s_slots[]'s address
// range -- never returned by bb_diag_section_find(), but the defensive
// bounds check must still fail safely (0) rather than dereference it.
void test_bb_diag_section_stream_elem_size_out_of_bounds_pointer_returns_zero(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = "ds.iteroob", .snap_desc = &s_ds_stream_desc, .iter = ds_iter_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));

    const bb_diag_section_t *found = bb_diag_section_find("ds.iteroob");
    TEST_ASSERT_NOT_NULL(found);

    const bb_diag_section_t *stray_below =
        (const bb_diag_section_t *)((const uint8_t *)found - 0x100000);
    TEST_ASSERT_EQUAL_UINT(0, bb_diag_section_stream_elem_size(stray_below));

    // Also exercise the other out-of-bounds arm (above the table, not just
    // below it) -- same fail-safe (0), distinct branch.
    const bb_diag_section_t *stray_above =
        (const bb_diag_section_t *)((const uint8_t *)found + 0x100000);
    TEST_ASSERT_EQUAL_UINT(0, bb_diag_section_stream_elem_size(stray_above));
}

// A bb_diag_section_t* that lands inside s_slots[]'s address range but not
// on a slot boundary (e.g. a caller that mis-derived the pointer) -- the
// defensive alignment check must also fail safely (0), distinct from the
// out-of-bounds case above.
void test_bb_diag_section_stream_elem_size_misaligned_pointer_returns_zero(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = "ds.itermisalign", .snap_desc = &s_ds_stream_desc, .iter = ds_iter_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));

    const bb_diag_section_t *found = bb_diag_section_find("ds.itermisalign");
    TEST_ASSERT_NOT_NULL(found);

    const bb_diag_section_t *misaligned = (const bb_diag_section_t *)((const uint8_t *)found + 1);
    TEST_ASSERT_EQUAL_UINT(0, bb_diag_section_stream_elem_size(misaligned));
}

void test_bb_diag_register_section_fill_with_stream_field_returns_invalid_arg(void)
{
    ds_reset();
    // fill (not iter) set, but snap_desc carries a BB_ARR_STREAM field --
    // a fill section has no two-phase arena to wire it against.
    bb_diag_section_t sec = { .name = "ds.fillstream", .snap_desc = &s_ds_stream_desc, .fill = ds_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_iter_zero_stream_fields_returns_invalid_arg(void)
{
    ds_reset();
    // iter set, but snap_desc has no BB_ARR_STREAM field at all.
    bb_diag_section_t sec = { .name = "ds.iternostream", .snap_desc = &s_ds_desc, .iter = ds_iter_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_iter_two_stream_fields_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = { .name = "ds.itertwostream", .snap_desc = &s_ds_two_stream_desc, .iter = ds_iter_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_iter_stream_elem_type_not_obj_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.iterstrelem", .snap_desc = &s_ds_stream_str_elem_desc, .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

void test_bb_diag_register_section_iter_stream_elem_size_exceeds_max_row_bytes_returns_no_space(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.iteroversizerow", .snap_desc = &s_ds_oversize_row_desc, .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_diag_register_section(&sec));
}

// A BB_ARR_STREAM field whose elem_size is 0 (forgot to set it) -- rejected
// BB_ERR_INVALID_ARG, distinct from the oversize-elem_size BB_ERR_NO_SPACE
// case above.
void test_bb_diag_register_section_iter_stream_elem_size_zero_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.iterzeroelemsize", .snap_desc = &s_ds_zero_elem_size_desc, .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

// A BB_ARR_STREAM field nested inside a top-level OBJ child's own children
// table -- rejected (no wiring point below the top level).
void test_bb_diag_register_section_iter_stream_nested_in_obj_child_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.iternestedobj", .snap_desc = &s_ds_nested_stream_in_obj_desc, .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

// A BB_ARR_STREAM field nested inside a top-level FIXED ARR-of-OBJ field's
// element row shape -- same reject, distinct recursion arm (ARR-of-OBJ
// rather than OBJ).
void test_bb_diag_register_section_iter_stream_nested_in_arr_of_obj_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.iternestedarr", .snap_desc = &s_ds_nested_stream_in_arr_of_obj_desc, .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

// A BB_ARR_STREAM field nested inside a top-level OBJ field's own
// ARR-of-OBJ child row shape -- reject_nested_stream_fields() itself (not
// just validate_stream_field()'s top-level loop) must recurse through an
// ARR-of-OBJ child, distinct from case 1's OBJ-child recursion.
void test_bb_diag_register_section_iter_stream_nested_via_mid_arr_of_obj_returns_invalid_arg(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.iternestedmidarr", .snap_desc = &s_ds_nested_stream_via_mid_arr_of_obj_desc,
        .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_register_section(&sec));
}

// A valid top-level STREAM field alongside a clean top-level OBJ sibling
// (no STREAM anywhere below it) -- reject_nested_stream_fields() must
// return BB_OK for the clean sibling and let the scan continue rather than
// short-circuit, and the section registers successfully.
void test_bb_diag_register_section_iter_stream_with_clean_sibling_obj_succeeds(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.iterstreamclean", .snap_desc = &s_ds_stream_with_clean_obj_desc, .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));
}

// A valid top-level STREAM field alongside a top-level FIXED ARR field
// whose elem_type is a scalar (not BB_TYPE_OBJ) -- neither the OBJ nor the
// ARR-of-OBJ arm of validate_stream_field()'s children-table check applies,
// so it's skipped without recursion, and the section still registers.
void test_bb_diag_register_section_iter_stream_with_scalar_arr_sibling_succeeds(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.iterstreamscalar", .snap_desc = &s_ds_stream_with_scalar_arr_desc, .iter = ds_iter_ok,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));
}

// A STREAM field nested 8 OBJ levels below the top -- at/past
// BB_SERIALIZE_MAX_DEPTH, reject_nested_stream_fields() fail-opens (mirrors
// the walker's own recursion guard: unreachable that deep, so validating it
// is pointless) and registration succeeds despite the buried STREAM field.
void test_bb_diag_register_section_fill_stream_nested_beyond_max_depth_fail_open_succeeds(void)
{
    ds_reset();
    bb_diag_section_t sec = {
        .name = "ds.depthcap", .snap_desc = &s_ds_depth_cap_desc, .fill = ds_fill_ok,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&sec));
}
