// test_bb_serialize_meta_validate — Unity coverage for
// bb_serialize_meta_validate(): happy path over the synthetic widget
// worked example (test_serialize_fixture.h) plus injected-mismatch cases.
// Mismatch fixtures are LOCAL static const tables built inside this file
// -- the production bb_fixture_widget_meta table is never mutated or
// exported for test-only use.

#include "unity.h"

#include "bb_serialize_meta.h"

#include "test_serialize_fixture.h"

#include <stddef.h>
#include <string.h>

extern const bb_serialize_desc_meta_t bb_fixture_widget_meta;

// ---------------------------------------------------------------------------
// 1. happy path
// ---------------------------------------------------------------------------

void test_bb_serialize_meta_validate_widget_happy_path(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &bb_fixture_widget_meta, err, sizeof err));
}

// ---------------------------------------------------------------------------
// 2. type_name mismatch
// ---------------------------------------------------------------------------

void test_bb_serialize_meta_validate_type_name_mismatch(void)
{
    bb_serialize_desc_meta_t bad_meta = bb_fixture_widget_meta;
    bad_meta.type_name = "not_widget";

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &bad_meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "type_name mismatch") != NULL);
}

// ---------------------------------------------------------------------------
// 3. missing row -- a required base field has no meta row
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_missing_row_rows[] = {
    { .key = "serial" },
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    // "tags" intentionally omitted.
};

void test_bb_serialize_meta_validate_missing_row(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_missing_row_rows, .n_rows = 6,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "tags: missing meta row") != NULL);
}

// ---------------------------------------------------------------------------
// 4. orphan row -- a meta row has no matching base field
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_orphan_row_rows[] = {
    { .key = "serial" },
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
    { .key = "not_a_real_field" },
};

void test_bb_serialize_meta_validate_orphan_row(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_orphan_row_rows, .n_rows = 8,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "not_a_real_field: orphan meta row") != NULL);
}

// ---------------------------------------------------------------------------
// 5. type/constraint mismatch -- min_len/enum_vals on a non-string field
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_string_violation_rows[] = {
    { .key = "serial" },
    { .key = "calibrated", .min_len = 5 },  // BOOL field -- invalid
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_string_constraint_on_non_string_field(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_string_violation_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "calibrated") != NULL);
    TEST_ASSERT_TRUE(strstr(err, "string-shaped") != NULL);
}

// ---------------------------------------------------------------------------
// 6. type/constraint mismatch -- has_min/has_max on a non-numeric field
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_numeric_violation_rows[] = {
    { .key = "serial", .has_min = true, .min = 0 },  // STR field -- invalid
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_numeric_constraint_on_non_numeric_field(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_numeric_violation_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "serial") != NULL);
    TEST_ASSERT_TRUE(strstr(err, "numeric") != NULL);
}

// ---------------------------------------------------------------------------
// 7. bad bounds -- has_min && has_max && min > max
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_bad_min_max_rows[] = {
    { .key = "serial" },
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s", .has_min = true, .min = 100, .has_max = true, .max = 0 },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_min_greater_than_max(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_bad_min_max_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "min > max") != NULL);
}

// ---------------------------------------------------------------------------
// 8. bad bounds -- min_len exceeds the base field's max_len
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_bad_min_len_rows[] = {
    { .key = "serial", .min_len = 100 },  // field max_len is 18
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_min_len_exceeds_field_max_len(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_bad_min_len_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "min_len exceeds") != NULL);
}

// ---------------------------------------------------------------------------
// 9/10. nested OBJ recursion -- synthetic fixture (the widget fixture is flat)
// ---------------------------------------------------------------------------

typedef struct {
    int64_t x;
} synth_child_t;

typedef struct {
    synth_child_t obj;
} synth_snap_t;

static const bb_serialize_field_t s_synth_child_fields[] = {
    { .key = "x", .type = BB_TYPE_I64, .offset = offsetof(synth_child_t, x) },
};

static const bb_serialize_field_t s_synth_fields[] = {
    { .key = "obj", .type = BB_TYPE_OBJ, .offset = offsetof(synth_snap_t, obj),
      .children = s_synth_child_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_synth_desc = {
    .type_name = "synth", .fields = s_synth_fields, .n_fields = 1,
    .snap_size = sizeof(synth_snap_t),
};

static const bb_serialize_field_meta_t s_synth_child_rows_ok[] = {
    { .key = "x", .has_min = true, .min = 0, .has_max = true, .max = 100 },
};

static const bb_serialize_field_meta_t s_synth_rows_ok[] = {
    { .key = "obj", .children = s_synth_child_rows_ok, .n_children = 1 },
};

static const bb_serialize_desc_meta_t s_synth_meta_ok = {
    .type_name = "synth", .rows = s_synth_rows_ok, .n_rows = 1,
};

void test_bb_serialize_meta_validate_nested_obj_happy_path(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&s_synth_desc, &s_synth_meta_ok, err, sizeof err));
}

// Nested child row keyed wrong -- the missing-row error must propagate up
// through the recursive call with the nested path prefix intact.
static const bb_serialize_field_meta_t s_synth_child_rows_bad[] = {
    { .key = "not_x" },
};

static const bb_serialize_field_meta_t s_synth_rows_bad[] = {
    { .key = "obj", .children = s_synth_child_rows_bad, .n_children = 1 },
};

static const bb_serialize_desc_meta_t s_synth_meta_bad = {
    .type_name = "synth", .rows = s_synth_rows_bad, .n_rows = 1,
};

void test_bb_serialize_meta_validate_nested_missing_row_propagates(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&s_synth_desc, &s_synth_meta_bad, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "synth.obj.x: missing meta row") != NULL);
}

// ---------------------------------------------------------------------------
// 11. depth guard -- self-referential OBJ child bails at BB_SERIALIZE_MAX_DEPTH
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

static const bb_serialize_field_meta_t s_deep_rows[] = {
    { .key = "marker" },
    { .key = "deep", .children = s_deep_rows, .n_children = 2 },
};

static const bb_serialize_desc_meta_t s_deep_meta = {
    .type_name = "deep", .rows = s_deep_rows, .n_rows = 2,
};

void test_bb_serialize_meta_validate_depth_guard_bails_on_self_reference(void)
{
    char err[128] = { 0 };
    // The depth guard bails silently (matches bb_serialize_walk()'s own
    // depth-cap semantics) -- no error, no stack overflow, no hang.
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&s_deep_desc, &s_deep_meta, err, sizeof err));
}

// ---------------------------------------------------------------------------
// 12. branch-coverage fixture -- combinations the widget fixture's flat
// fields never exercise: U64/F64 numeric-type detection, has_min-only /
// has_max-only, a STR_N field with min_len set but no field-level max_len,
// an enum-only (min_len unset) string field, and an ARR-of-OBJ field (vs.
// the widget fixture's ARR-of-STR tags). All agree with their meta --
// expected BB_OK.
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_cov_arrobj_child_fields[] = {
    { .key = "leaf", .type = BB_TYPE_I64 },
};

static const bb_serialize_field_t s_cov_fields_full[] = {
    { .key = "u64f", .type = BB_TYPE_U64 },
    { .key = "f64f", .type = BB_TYPE_F64 },
    { .key = "hasminonly", .type = BB_TYPE_I64 },
    { .key = "hasmaxonly", .type = BB_TYPE_I64 },
    { .key = "strn_minlen", .type = BB_TYPE_STR_N },
    { .key = "enumonly", .type = BB_TYPE_STR, .max_len = 20 },
    { .key = "arrobj", .type = BB_TYPE_ARR, .elem_type = BB_TYPE_OBJ,
      .children = s_cov_arrobj_child_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_cov_desc = {
    .type_name = "cov", .fields = s_cov_fields_full, .n_fields = 7,
};

static const char *const s_cov_enum_vals[] = { "a", "b", NULL };

static const bb_serialize_field_meta_t s_cov_arrobj_child_rows[] = {
    { .key = "leaf" },
};

static const bb_serialize_field_meta_t s_cov_rows[] = {
    { .key = "u64f" },
    { .key = "f64f" },
    { .key = "hasminonly", .has_min = true, .min = 0 },
    { .key = "hasmaxonly", .has_max = true, .max = 100 },
    { .key = "strn_minlen", .min_len = 5 },  // field max_len is 0 (unset) -- no bounds check
    { .key = "enumonly", .enum_vals = s_cov_enum_vals },  // min_len unset, enum set
    { .key = "arrobj", .children = s_cov_arrobj_child_rows, .n_children = 1 },
};

static const bb_serialize_desc_meta_t s_cov_meta = {
    .type_name = "cov", .rows = s_cov_rows, .n_rows = 7,
};

void test_bb_serialize_meta_validate_coverage_fixture_happy_path(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&s_cov_desc, &s_cov_meta, err, sizeof err));
}

// ---------------------------------------------------------------------------
// 13. duplicate row -- a key listed twice (n_rows inflated to match) must
// fail, not silently pass through the missing-row/orphan-row bijection gap.
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_duplicate_row_rows[] = {
    { .key = "serial" },
    { .key = "serial" },  // duplicate
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_duplicate_row(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_duplicate_row_rows, .n_rows = 8,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "duplicate meta row for key 'serial'") != NULL);
}

// ---------------------------------------------------------------------------
// 14. NULL key on a meta row -- must fail cleanly, never strcmp(NULL, ...).
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_null_key_rows[] = {
    { .key = NULL },
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_null_key_row(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_null_key_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "missing key") != NULL);
}

// ---------------------------------------------------------------------------
// 15+. B1-1181a -- duplicate-key "oneOf" / occurrence-tagged rows. LOCAL
// synthetic fixtures only (never the production reboot/storage_delete
// tables -- see test_bb_system_reboot_meta_golden.c /
// test_bb_storage_http_delete_apply_meta_golden.c for those).
// ---------------------------------------------------------------------------

typedef struct {
    char dup_str[8];
    bb_serialize_arr_t dup_arr;
} val_oneof_snap_t;

static const bb_serialize_field_t s_val_oneof_fields[] = {
    { .key = "dup", .type = BB_TYPE_STR, .offset = offsetof(val_oneof_snap_t, dup_str),
      .max_len = sizeof(((val_oneof_snap_t *)0)->dup_str) },
    { .key = "dup", .type = BB_TYPE_ARR, .elem_type = BB_TYPE_STR,
      .offset = offsetof(val_oneof_snap_t, dup_arr) },
};

static const bb_serialize_desc_t s_val_oneof_desc = {
    .type_name = "val_oneof", .fields = s_val_oneof_fields, .n_fields = 2,
    .snap_size = sizeof(val_oneof_snap_t),
};

static const bb_serialize_field_meta_t s_val_oneof_branch_rows[] = {
    { .key = "dup" },
    { .key = "dup" },
};

static const bb_serialize_field_meta_t *const s_val_oneof_branches[] = {
    &s_val_oneof_branch_rows[0], &s_val_oneof_branch_rows[1], NULL,
};

// 15. Happy path -- a clean 2-branch oneOf key.
static const bb_serialize_field_meta_t s_val_oneof_rows_ok[] = {
    { .key = "dup", .kind = BB_SERIALIZE_META_KIND_ONEOF,
      .branches = s_val_oneof_branches, .n_branches = 2 },
};

static const bb_serialize_desc_meta_t s_val_oneof_meta_ok = {
    .type_name = "val_oneof", .rows = s_val_oneof_rows_ok, .n_rows = 1,
};

void test_bb_serialize_meta_validate_oneof_two_branches_happy_path(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&s_val_oneof_desc, &s_val_oneof_meta_ok, err, sizeof err));
}

// 16. Happy path -- a reboot-shaped duplicate key where only ONE physical
// occurrence is ever meant to be documented/surfaced (occurrence-tagged
// FIELD row, kind defaults to BB_SERIALIZE_META_KIND_FIELD).
typedef struct {
    uint64_t ts_real;
    double   ts_shadow;
} val_occ_snap_t;

static const bb_serialize_field_t s_val_occ_fields[] = {
    { .key = "ts", .type = BB_TYPE_U64, .offset = offsetof(val_occ_snap_t, ts_real) },
    { .key = "ts", .type = BB_TYPE_F64, .offset = offsetof(val_occ_snap_t, ts_shadow) },
};

static const bb_serialize_desc_t s_val_occ_desc = {
    .type_name = "val_occ", .fields = s_val_occ_fields, .n_fields = 2,
    .snap_size = sizeof(val_occ_snap_t),
};

static const bb_serialize_field_meta_t s_val_occ_rows_ok[] = {
    { .key = "ts", .occurrence = 0 },
};

static const bb_serialize_desc_meta_t s_val_occ_meta_ok = {
    .type_name = "val_occ", .rows = s_val_occ_rows_ok, .n_rows = 1,
};

void test_bb_serialize_meta_validate_occurrence_tagged_reboot_shaped_happy_path(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&s_val_occ_desc, &s_val_occ_meta_ok, err, sizeof err));
}

// 17. Failure -- oneOf branch-count mismatch (n_branches != occurrence count).
static const bb_serialize_field_meta_t s_val_oneof_rows_bad_count[] = {
    { .key = "dup", .kind = BB_SERIALIZE_META_KIND_ONEOF,
      .branches = s_val_oneof_branches, .n_branches = 1 },  // desc has 2 occurrences
};

static const bb_serialize_desc_meta_t s_val_oneof_meta_bad_count = {
    .type_name = "val_oneof", .rows = s_val_oneof_rows_bad_count, .n_rows = 1,
};

void test_bb_serialize_meta_validate_oneof_branch_count_mismatch(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&s_val_oneof_desc, &s_val_oneof_meta_bad_count, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "oneOf n_branches") != NULL);
}

// 18. Failure -- occurrence-tagged FIELD row's `.occurrence` is out of range
// (>= the key's physical occurrence count).
static const bb_serialize_field_meta_t s_val_occ_rows_out_of_range[] = {
    { .key = "ts", .occurrence = 2 },  // only occurrences 0/1 exist
};

static const bb_serialize_desc_meta_t s_val_occ_meta_out_of_range = {
    .type_name = "val_occ", .rows = s_val_occ_rows_out_of_range, .n_rows = 1,
};

void test_bb_serialize_meta_validate_occurrence_out_of_range(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&s_val_occ_desc, &s_val_occ_meta_out_of_range, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "occurrence 2 out of range") != NULL);
}

// 19. Failure -- an orphan oneOf branch (a branch row whose `.key` doesn't
// match the parent oneOf row's key -- a copy-paste mistake pointing at the
// wrong branch table).
static const bb_serialize_field_meta_t s_val_oneof_orphan_branch_rows[] = {
    { .key = "dup" },
    { .key = "not_dup" },  // orphan -- key mismatch
};

static const bb_serialize_field_meta_t *const s_val_oneof_orphan_branches[] = {
    &s_val_oneof_orphan_branch_rows[0], &s_val_oneof_orphan_branch_rows[1], NULL,
};

static const bb_serialize_field_meta_t s_val_oneof_rows_orphan[] = {
    { .key = "dup", .kind = BB_SERIALIZE_META_KIND_ONEOF,
      .branches = s_val_oneof_orphan_branches, .n_branches = 2 },
};

static const bb_serialize_desc_meta_t s_val_oneof_meta_orphan = {
    .type_name = "val_oneof", .rows = s_val_oneof_rows_orphan, .n_rows = 1,
};

void test_bb_serialize_meta_validate_oneof_orphan_branch(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&s_val_oneof_desc, &s_val_oneof_meta_orphan, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "orphan oneOf branch") != NULL);
}

// 20. Failure -- a ONEOF row with `.branches == NULL`.
static const bb_serialize_field_meta_t s_val_oneof_rows_no_branches[] = {
    { .key = "dup", .kind = BB_SERIALIZE_META_KIND_ONEOF, .branches = NULL, .n_branches = 2 },
};

static const bb_serialize_desc_meta_t s_val_oneof_meta_no_branches = {
    .type_name = "val_oneof", .rows = s_val_oneof_rows_no_branches, .n_rows = 1,
};

void test_bb_serialize_meta_validate_oneof_missing_branches(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&s_val_oneof_desc, &s_val_oneof_meta_no_branches, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "oneOf row missing branches") != NULL);
}

// 21. Failure -- one oneOf branch pointer is NULL.
static const bb_serialize_field_meta_t s_val_oneof_one_null_branch_rows[] = {
    { .key = "dup" },
};

static const bb_serialize_field_meta_t *const s_val_oneof_one_null_branches[] = {
    &s_val_oneof_one_null_branch_rows[0], NULL, NULL,
};

static const bb_serialize_field_meta_t s_val_oneof_rows_null_branch[] = {
    { .key = "dup", .kind = BB_SERIALIZE_META_KIND_ONEOF,
      .branches = s_val_oneof_one_null_branches, .n_branches = 2 },
};

static const bb_serialize_desc_meta_t s_val_oneof_meta_null_branch = {
    .type_name = "val_oneof", .rows = s_val_oneof_rows_null_branch, .n_rows = 1,
};

void test_bb_serialize_meta_validate_oneof_null_branch_entry(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&s_val_oneof_desc, &s_val_oneof_meta_null_branch, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "oneOf branch 1 is NULL") != NULL);
}

// 22. Failure -- accidental unannotated duplicate key: NO meta row at all
// for a key with occurrence_count > 1 (the "copy-paste dup still caught"
// invariant -- must NOT silently pass through the relaxed bijection).
static const bb_serialize_desc_meta_t s_val_oneof_meta_no_row_at_all = {
    .type_name = "val_oneof", .rows = NULL, .n_rows = 0,
};

void test_bb_serialize_meta_validate_duplicate_key_with_no_annotation_rejected(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&s_val_oneof_desc, &s_val_oneof_meta_no_row_at_all,
                                    err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "duplicate key with no oneOf/occurrence annotation") != NULL);
}

// 23. Failure -- non-contiguous duplicate key (review finding): [X, Y, X]
// -- the two physical "dup" occurrences are NOT back-to-back. Both the
// ONEOF-branch matching and the occurrence-tagged FIELD path assume
// contiguity (see bb_serialize_field_meta_s's "duplicate-key" doc); an
// interleaved table must be REJECTED, never silently pass with the
// second/later occurrence's row left NULL (which would look identical to
// the intentional-undocumented-occurrence case).
typedef struct {
    char   dup0[8];
    bool   middle;
    char   dup1[8];
} val_interleaved_snap_t;

static const bb_serialize_field_t s_val_interleaved_fields[] = {
    { .key = "dup", .type = BB_TYPE_STR, .offset = offsetof(val_interleaved_snap_t, dup0),
      .max_len = sizeof(((val_interleaved_snap_t *)0)->dup0) },
    { .key = "middle", .type = BB_TYPE_BOOL, .offset = offsetof(val_interleaved_snap_t, middle) },
    { .key = "dup", .type = BB_TYPE_STR, .offset = offsetof(val_interleaved_snap_t, dup1),
      .max_len = sizeof(((val_interleaved_snap_t *)0)->dup1) },
};

static const bb_serialize_desc_t s_val_interleaved_desc = {
    .type_name = "val_interleaved", .fields = s_val_interleaved_fields, .n_fields = 3,
    .snap_size = sizeof(val_interleaved_snap_t),
};

// Meta table content is irrelevant here -- the contiguity guard fires
// before any row lookup -- but supply a plausible-looking occurrence-
// tagged row + a "middle" row so a bug regressing the guard's ORDERING
// (e.g. running it after row-matching) wouldn't accidentally still pass.
static const bb_serialize_field_meta_t s_val_interleaved_rows[] = {
    { .key = "dup", .occurrence = 0 },
    { .key = "middle" },
};

static const bb_serialize_desc_meta_t s_val_interleaved_meta = {
    .type_name = "val_interleaved", .rows = s_val_interleaved_rows, .n_rows = 2,
};

void test_bb_serialize_meta_validate_non_contiguous_duplicate_key_rejected(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&s_val_interleaved_desc, &s_val_interleaved_meta,
                                    err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "non-contiguous duplicate key") != NULL);
}

// ---------------------------------------------------------------------------
// 24. B1-1186 -- max_len on a non-string field is rejected (same class as
// min_len/enum_vals's existing check, now folded together).
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_max_len_on_bool_rows[] = {
    { .key = "serial" },
    { .key = "calibrated", .max_len = 5 },  // BOOL field -- invalid
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_max_len_on_non_string_field(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_max_len_on_bool_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "calibrated") != NULL);
    TEST_ASSERT_TRUE(strstr(err, "string-shaped") != NULL);
}

// ---------------------------------------------------------------------------
// 25. B1-1186 -- bad bounds: min_len > max_len on the same field.
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_bad_min_len_max_len_rows[] = {
    { .key = "serial", .min_len = 10, .max_len = 5 },  // min_len > max_len
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_min_len_greater_than_max_len(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_bad_min_len_max_len_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "min_len exceeds max_len") != NULL);
}

// ---------------------------------------------------------------------------
// 26. B1-1186 -- bad bounds: max_len exceeds the base field's own max_len
// (the schema bound can never claim to accept MORE than the wire buffer
// physically holds).
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_bad_max_len_rows[] = {
    { .key = "serial", .max_len = 100 },  // field max_len is 18
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_max_len_exceeds_field_max_len(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_bad_max_len_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
    TEST_ASSERT_TRUE(strstr(err, "max_len exceeds field max_len") != NULL);
}

// ---------------------------------------------------------------------------
// 27. B1-1186 -- happy path: max_len set within the base field's max_len,
// combined with an existing min_len on the same field.
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t s_good_min_max_len_rows[] = {
    { .key = "serial", .min_len = 5, .max_len = 17 },  // field max_len is 18
    { .key = "calibrated" },
    { .key = "armed" },
    { .key = "installed_epoch_s" },
    { .key = "region" },
    { .key = "label" },
    { .key = "tags" },
};

void test_bb_serialize_meta_validate_min_len_and_max_len_happy_path(void)
{
    static const bb_serialize_desc_meta_t meta = {
        .type_name = "widget", .rows = s_good_min_max_len_rows, .n_rows = 7,
    };

    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_fixture_widget_desc, &meta, err, sizeof err));
}
