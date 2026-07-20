// Host tests for the bb_health_section composer registry (B1-1096 PR-1 of
// 6, epic B1-1054): bb_health_section_register() plus
// bb_health_section_freeze(). ADDITIVE AND INERT -- nothing consumes this
// registry yet; these tests exercise the registry contract in isolation,
// mirroring test_bb_diag_section.c's fixture idiom.

#include "unity.h"

#include "bb_health_section_priv.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixture: a tiny snapshot type + fake fill hook.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} hs_snap_t;

static const bb_serialize_field_t s_hs_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(hs_snap_t, n) },
};

static const bb_serialize_desc_t s_hs_desc = {
    .type_name = "hs_snap_t",
    .fields    = s_hs_fields,
    .n_fields  = 1,
    .snap_size = sizeof(hs_snap_t),
};

// A snap_desc whose snap_size deliberately exceeds
// BB_HEALTH_SECTION_SCRATCH_BYTES (128 by default, no Kconfig override in
// the host build) -- used only to exercise
// bb_health_section_register()'s loud attach-time reject; never actually
// filled.
typedef struct {
    char big[BB_HEALTH_SECTION_SCRATCH_BYTES + 1];
} hs_oversize_snap_t;

static const bb_serialize_desc_t s_hs_oversize_desc = {
    .type_name = "hs_oversize_snap_t",
    .fields    = NULL,
    .n_fields  = 0,
    .snap_size = sizeof(hs_oversize_snap_t),
};

static bb_err_t hs_fill_ok(void *dst, const bb_health_fill_args_t *args)
{
    ((hs_snap_t *)dst)->n = *(int64_t *)args->ctx;
    return BB_OK;
}

static bb_err_t hs_fill_fail(void *dst, const bb_health_fill_args_t *args)
{
    (void)dst;
    (void)args;
    return BB_ERR_INVALID_STATE;
}

static void hs_reset(void)
{
    bb_health_section_test_reset();
}

// ---------------------------------------------------------------------------
// bb_health_section_register
// ---------------------------------------------------------------------------

void test_bb_health_section_register_success(void)
{
    hs_reset();
    int64_t ctx_val = 1;
    bb_health_section_t sec = { .name = "hs.ok", .snap_desc = &s_hs_desc,
                                .fill = hs_fill_ok, .ctx = &ctx_val };
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&sec));
}

void test_bb_health_section_register_success_schema_props_round_trips(void)
{
    hs_reset();
    static const char s_schema[] = "{\"type\":\"object\"}";
    int64_t ctx_val = 1;
    bb_health_section_t sec = { .name = "hs.schema", .snap_desc = &s_hs_desc,
                                 .fill = hs_fill_ok, .ctx = &ctx_val,
                                 .schema_props = s_schema };
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&sec));

    const bb_health_section_t *stored = bb_health_section_test_find("hs.schema");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_PTR(s_schema, stored->schema_props);
}

void test_bb_health_section_register_null_section_returns_invalid_arg(void)
{
    hs_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_health_section_register(NULL));
}

void test_bb_health_section_register_null_name_returns_invalid_arg(void)
{
    hs_reset();
    bb_health_section_t sec = { .name = NULL, .snap_desc = &s_hs_desc, .fill = hs_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_health_section_register(&sec));
}

void test_bb_health_section_register_null_snap_desc_returns_invalid_arg(void)
{
    hs_reset();
    bb_health_section_t sec = { .name = "hs.nodesc", .snap_desc = NULL, .fill = hs_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_health_section_register(&sec));
}

void test_bb_health_section_register_null_fill_returns_invalid_arg(void)
{
    hs_reset();
    bb_health_section_t sec = { .name = "hs.nofill", .snap_desc = &s_hs_desc, .fill = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_health_section_register(&sec));
}

void test_bb_health_section_register_name_too_long_returns_invalid_arg(void)
{
    hs_reset();
    char name_over[BB_HEALTH_SECTION_NAME_MAX + 1];
    memset(name_over, 'n', sizeof(name_over) - 1);
    name_over[sizeof(name_over) - 1] = '\0';  // strlen == BB_HEALTH_SECTION_NAME_MAX, over

    bb_health_section_t sec = { .name = name_over, .snap_desc = &s_hs_desc, .fill = hs_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_health_section_register(&sec));
}

void test_bb_health_section_register_name_max_boundary_ok(void)
{
    hs_reset();
    char name_ok[BB_HEALTH_SECTION_NAME_MAX];
    memset(name_ok, 'n', sizeof(name_ok) - 1);
    name_ok[sizeof(name_ok) - 1] = '\0';  // strlen == BB_HEALTH_SECTION_NAME_MAX - 1, fits

    bb_health_section_t sec = { .name = name_ok, .snap_desc = &s_hs_desc, .fill = hs_fill_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&sec));
}

void test_bb_health_section_register_duplicate_name_returns_invalid_state(void)
{
    hs_reset();
    bb_health_section_t a = { .name = "hs.dup", .snap_desc = &s_hs_desc, .fill = hs_fill_ok };
    bb_health_section_t b = { .name = "hs.dup", .snap_desc = &s_hs_desc, .fill = hs_fill_fail };
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&a));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_health_section_register(&b));
}

void test_bb_health_section_register_table_full_returns_no_space(void)
{
    hs_reset();
    char names[BB_HEALTH_SECTION_TABLE_CAP + 1][BB_HEALTH_SECTION_NAME_MAX];
    for (int i = 0; i < BB_HEALTH_SECTION_TABLE_CAP; i++) {
        snprintf(names[i], sizeof(names[i]), "hs.cap.%d", i);
        bb_health_section_t sec = { .name = names[i], .snap_desc = &s_hs_desc, .fill = hs_fill_ok };
        TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&sec));
    }

    snprintf(names[BB_HEALTH_SECTION_TABLE_CAP], sizeof(names[BB_HEALTH_SECTION_TABLE_CAP]),
             "hs.cap.%d", BB_HEALTH_SECTION_TABLE_CAP);
    bb_health_section_t overflow = {
        .name = names[BB_HEALTH_SECTION_TABLE_CAP], .snap_desc = &s_hs_desc, .fill = hs_fill_ok,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_health_section_register(&overflow));
}

void test_bb_health_section_register_duplicate_name_wins_over_table_full(void)
{
    hs_reset();
    char names[BB_HEALTH_SECTION_TABLE_CAP][BB_HEALTH_SECTION_NAME_MAX];
    for (int i = 0; i < BB_HEALTH_SECTION_TABLE_CAP; i++) {
        snprintf(names[i], sizeof(names[i]), "hs.dupcap.%d", i);
        bb_health_section_t sec = { .name = names[i], .snap_desc = &s_hs_desc, .fill = hs_fill_ok };
        TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&sec));
    }

    // Table is now full; re-registering an ALREADY-bound name still reports
    // the duplicate as BB_ERR_INVALID_STATE, not BB_ERR_NO_SPACE.
    bb_health_section_t redup = { .name = names[0], .snap_desc = &s_hs_desc, .fill = hs_fill_fail };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_health_section_register(&redup));
}

void test_bb_health_section_register_snap_size_exceeds_scratch_returns_no_space(void)
{
    hs_reset();
    bb_health_section_t sec = { .name = "hs.oversize", .snap_desc = &s_hs_oversize_desc, .fill = hs_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_health_section_register(&sec));
}

// ---------------------------------------------------------------------------
// bb_health_section_freeze
// ---------------------------------------------------------------------------

void test_bb_health_section_register_after_freeze_returns_invalid_state(void)
{
    hs_reset();
    bb_health_section_freeze();
    bb_health_section_t sec = { .name = "hs.frozen", .snap_desc = &s_hs_desc, .fill = hs_fill_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_health_section_register(&sec));
}

void test_bb_health_section_register_before_freeze_still_ok(void)
{
    hs_reset();
    bb_health_section_t sec = { .name = "hs.prefreeze", .snap_desc = &s_hs_desc, .fill = hs_fill_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&sec));
    // Registering before freeze is unaffected by a LATER freeze call.
    bb_health_section_freeze();
}
