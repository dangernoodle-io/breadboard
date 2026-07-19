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
