// test_bb_serialize_meta_openapi — Unity coverage for
// bb_serialize_meta_openapi_schema(): a golden-string assertion for the
// bb_info worked example, plus synthetic fixtures (nested OBJ, ARR-of-OBJ,
// depth-cap, missing-meta, overflow) exercising every composer branch. The
// synthetic fixtures are LOCAL to this file -- production tables are never
// mutated.

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_info/bb_info_wire_priv.h"

#include <stddef.h>
#include <string.h>

extern const bb_serialize_desc_meta_t bb_info_wire_meta;

// ---------------------------------------------------------------------------
// 1. golden -- bb_info worked example
// ---------------------------------------------------------------------------

void test_bb_serialize_meta_openapi_info_golden(void)
{
    char   buf[1536];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_info_wire_desc, &bb_info_wire_meta, buf, sizeof buf, &n));

    const char *golden =
        "{\"type\":\"object\",\"properties\":{"
        "\"mac\":{\"type\":\"string\",\"minLength\":17,\"title\":\"MAC address\","
        "\"description\":\"Device station MAC, colon-separated hex\",\"format\":\"mac\","
        "\"examples\":[\"aa:bb:cc:dd:ee:ff\"]},"
        "\"ota_validated\":{\"type\":\"boolean\",\"title\":\"OTA validated\","
        "\"description\":\"True once the running image passed its OTA validation check\"},"
        "\"time_valid\":{\"type\":\"boolean\",\"title\":\"Time valid\","
        "\"description\":\"True once the device clock has been synchronized\"},"
        "\"boot_epoch_s\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4.10244e+09,"
        "\"title\":\"Boot epoch\","
        "\"description\":\"Unix time (seconds) the device booted, or 0 if unknown\","
        "\"examples\":[1704067200]},"
        "\"time_source\":{\"type\":\"string\",\"minLength\":4,\"title\":\"Time source\","
        "\"description\":\"Clock synchronization source\",\"examples\":[\"sntp\"],"
        "\"enum\":[\"sntp\",\"none\"]},"
        "\"hostname\":{\"type\":\"string\",\"title\":\"Hostname\","
        "\"description\":\"mDNS hostname, or null if not yet assigned\","
        "\"examples\":[\"bb-test\"]},"
        "\"capabilities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
        "\"title\":\"Capabilities\","
        "\"description\":\"Composed-in optional-feature identifiers\"}"
        "},\"required\":[\"mac\",\"ota_validated\",\"time_valid\",\"boot_epoch_s\","
        "\"time_source\",\"capabilities\"],\"additionalProperties\":false}";

    TEST_ASSERT_EQUAL_STRING(golden, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(golden), n);
}

// ---------------------------------------------------------------------------
// 2. missing meta row -- field with no matching row (and meta == NULL)
// ---------------------------------------------------------------------------

typedef struct {
    bool lonely;
} lonely_snap_t;

static const bb_serialize_field_t s_lonely_fields[] = {
    { .key = "lonely", .type = BB_TYPE_BOOL, .offset = offsetof(lonely_snap_t, lonely) },
};

static const bb_serialize_desc_t s_lonely_desc = {
    .type_name = "lonely", .fields = s_lonely_fields, .n_fields = 1,
    .snap_size = sizeof(lonely_snap_t),
};

void test_bb_serialize_meta_openapi_no_meta_at_all(void)
{
    char   buf[256];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&s_lonely_desc, NULL, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"object\",\"properties\":{\"lonely\":{\"type\":\"boolean\"}},"
        "\"required\":[],\"additionalProperties\":false}", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), n);
}

static const bb_serialize_field_meta_t s_lonely_rows_empty[] = { { .key = "not_lonely" } };

static const bb_serialize_desc_meta_t s_lonely_meta_empty = {
    .type_name = "lonely", .rows = s_lonely_rows_empty, .n_rows = 1,
};

void test_bb_serialize_meta_openapi_no_matching_row(void)
{
    char   buf[256];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&s_lonely_desc, &s_lonely_meta_empty, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"object\",\"properties\":{\"lonely\":{\"type\":\"boolean\"}},"
        "\"required\":[],\"additionalProperties\":false}", buf);
}

// ---------------------------------------------------------------------------
// 3. nested OBJ -- present-but-sparse child row (row != NULL, no docs set)
// ---------------------------------------------------------------------------

typedef struct {
    int64_t x;
} nested_child_t;

typedef struct {
    nested_child_t obj;
} nested_snap_t;

static const bb_serialize_field_t s_nested_child_fields[] = {
    { .key = "x", .type = BB_TYPE_I64, .offset = offsetof(nested_child_t, x) },
};

static const bb_serialize_field_t s_nested_fields[] = {
    { .key = "obj", .type = BB_TYPE_OBJ, .offset = offsetof(nested_snap_t, obj),
      .children = s_nested_child_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_nested_desc = {
    .type_name = "nested", .fields = s_nested_fields, .n_fields = 1,
    .snap_size = sizeof(nested_snap_t),
};

// Child row present (row != NULL) but every doc/enum field left unset --
// exercises the false branch of each `if (row->x)` inside write_docs, and
// the `!row->required` (row present, required false) skip in the nested
// required-array pass.
static const bb_serialize_field_meta_t s_nested_child_rows[] = {
    { .key = "x" },
};

static const bb_serialize_field_meta_t s_nested_rows[] = {
    { .key = "obj", .required = true,
      .children = s_nested_child_rows, .n_children = 1 },
};

static const bb_serialize_desc_meta_t s_nested_meta = {
    .type_name = "nested", .rows = s_nested_rows, .n_rows = 1,
};

void test_bb_serialize_meta_openapi_nested_obj(void)
{
    char   buf[512];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&s_nested_desc, &s_nested_meta, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"object\",\"properties\":{\"obj\":{\"type\":\"object\","
        "\"properties\":{\"x\":{\"type\":\"integer\"}},\"required\":[],"
        "\"additionalProperties\":false}},\"required\":[\"obj\"],"
        "\"additionalProperties\":false}", buf);
}

// ---------------------------------------------------------------------------
// 4. depth cap -- self-referential OBJ chain bails at BB_SERIALIZE_MAX_DEPTH
// ---------------------------------------------------------------------------

typedef struct {
    int64_t marker;
} deep_snap_t;

static const bb_serialize_field_t s_deep_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_snap_t, marker) },
    { .key = "deep", .type = BB_TYPE_OBJ, .offset = 0,
      .children = s_deep_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_desc = {
    .type_name = "deep", .fields = s_deep_fields, .n_fields = 2,
    .snap_size = sizeof(deep_snap_t),
};

void test_bb_serialize_meta_openapi_obj_depth_guard(void)
{
    char   buf[4096];
    size_t n = 0;

    // No error, no stack overflow, no hang -- the depth guard silently
    // bails on the "deep" field's own recursion once depth == MAX_DEPTH.
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&s_deep_desc, NULL, buf, sizeof buf, &n));
    TEST_ASSERT_TRUE(n > 0);
}

// ---------------------------------------------------------------------------
// 5. depth cap -- self-referential ARR-of-OBJ chain (write_items' own guard)
// ---------------------------------------------------------------------------

typedef struct {
    const void *items;
    size_t      count;
} arr_deep_snap_t;

static const bb_serialize_field_t s_arr_deep_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .elem_type = BB_TYPE_OBJ,
      .offset = 0, .children = s_arr_deep_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_arr_deep_desc = {
    .type_name = "arr_deep", .fields = s_arr_deep_fields, .n_fields = 1,
    .snap_size = sizeof(arr_deep_snap_t),
};

void test_bb_serialize_meta_openapi_arr_of_obj_depth_guard(void)
{
    char   buf[4096];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&s_arr_deep_desc, NULL, buf, sizeof buf, &n));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(strstr(buf, "\"type\":\"array\"") != NULL);
}

// ---------------------------------------------------------------------------
// 6. bounded-buffer overflow -- all-or-nothing, mirrors bb_serialize_json
// ---------------------------------------------------------------------------

void test_bb_serialize_meta_openapi_render_cap_zero(void)
{
    char buf[4] = { 'x', 'x', 'x', 'x' };
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE,
        bb_serialize_meta_openapi_schema(&bb_info_wire_desc, &bb_info_wire_meta, buf, 0, NULL));
}

void test_bb_serialize_meta_openapi_overflow_too_small_cap(void)
{
    char   buf[8];
    size_t n = 12345;

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE,
        bb_serialize_meta_openapi_schema(&bb_info_wire_desc, &bb_info_wire_meta, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_serialize_meta_openapi_overflow_null_out_len(void)
{
    char buf[8];
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE,
        bb_serialize_meta_openapi_schema(&bb_info_wire_desc, &bb_info_wire_meta, buf, sizeof buf, NULL));
}

void test_bb_serialize_meta_openapi_success_null_out_len(void)
{
    char buf[1024];
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&s_lonely_desc, NULL, buf, sizeof buf, NULL));
}

// ---------------------------------------------------------------------------
// 7. branch-coverage fixture -- combinations the golden/nested/depth-guard
// fixtures above don't reach: U64/F64 type-switch cases, a title needing
// quote/backslash escaping, a multi-entry "examples" array (comma path),
// an ARR-of-OBJ field with a REAL (non-NULL) meta row, and an OBJ field
// whose children mix "has a row" / "has no row" / "required" so both the
// properties-comma and required-comma paths fire.
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_cov_oa_obj_child_fields[] = {
    { .key = "a", .type = BB_TYPE_BOOL },
    { .key = "b", .type = BB_TYPE_BOOL },
    { .key = "c", .type = BB_TYPE_BOOL },  // intentionally no matching meta row
};

static const bb_serialize_field_t s_cov_oa_arr_child_fields[] = {
    { .key = "p", .type = BB_TYPE_I64 },
    { .key = "q", .type = BB_TYPE_I64 },
};

static const bb_serialize_field_t s_cov_oa_fields[] = {
    { .key = "u64f", .type = BB_TYPE_U64 },
    { .key = "f64f", .type = BB_TYPE_F64 },
    { .key = "esc", .type = BB_TYPE_STR },
    { .key = "objf", .type = BB_TYPE_OBJ,
      .children = s_cov_oa_obj_child_fields, .n_children = 3 },
    { .key = "arrf", .type = BB_TYPE_ARR, .elem_type = BB_TYPE_OBJ,
      .children = s_cov_oa_arr_child_fields, .n_children = 2 },
    { .key = "reff", .type = BB_TYPE_REF, .ref_key = "some.sibling" },
};

static const bb_serialize_desc_t s_cov_oa_desc = {
    .type_name = "cov_oa", .fields = s_cov_oa_fields, .n_fields = 6,
};

static const char *const s_cov_oa_examples[] = { "\"a\"", "\"b\"", NULL };

static const bb_serialize_field_meta_t s_cov_oa_obj_child_rows[] = {
    { .key = "a", .required = true },
    { .key = "b", .required = true },
    // "c" intentionally has no matching row.
};

static const bb_serialize_field_meta_t s_cov_oa_arr_child_rows[] = {
    { .key = "p" },
    { .key = "q" },
};

static const bb_serialize_field_meta_t s_cov_oa_rows[] = {
    { .key = "u64f" },
    { .key = "f64f" },
    { .key = "esc", .title = "Say \"hi\" \\ ok", .examples = s_cov_oa_examples },
    { .key = "objf", .children = s_cov_oa_obj_child_rows, .n_children = 2 },
    { .key = "arrf", .children = s_cov_oa_arr_child_rows, .n_children = 2 },
};

static const bb_serialize_desc_meta_t s_cov_oa_meta = {
    .type_name = "cov_oa", .rows = s_cov_oa_rows, .n_rows = 5,
};

void test_bb_serialize_meta_openapi_coverage_fixture(void)
{
    char   buf[2048];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&s_cov_oa_desc, &s_cov_oa_meta, buf, sizeof buf, &n));

    TEST_ASSERT_TRUE(strstr(buf, "\"u64f\":{\"type\":\"integer\"}") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "\"f64f\":{\"type\":\"number\"}") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "\"title\":\"Say \\\"hi\\\" \\\\ ok\"") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "\"examples\":[\"a\",\"b\"]") != NULL);
    // objf: "a"/"b" required (comma between them), "c" present but not required.
    TEST_ASSERT_TRUE(strstr(buf, "\"properties\":{\"a\":{\"type\":\"boolean\"},"
                                  "\"b\":{\"type\":\"boolean\"},\"c\":{\"type\":\"boolean\"}}") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "\"required\":[\"a\",\"b\"]") != NULL);
    // arrf: real (non-NULL) meta row driving the ARR-of-OBJ items schema.
    TEST_ASSERT_TRUE(strstr(buf, "\"items\":{\"type\":\"object\",\"properties\":"
                                  "{\"p\":{\"type\":\"integer\"},\"q\":{\"type\":\"integer\"}}") != NULL);
    // reff: BB_TYPE_REF documented as an opaque object (no static properties
    // expansion -- the sibling descriptor isn't known here).
    TEST_ASSERT_TRUE(strstr(buf, "\"reff\":{\"type\":\"object\"}") != NULL);
}
