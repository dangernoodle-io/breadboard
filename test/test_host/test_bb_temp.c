// Tests for bb_temp -- exercises bb_temp_read_soc(), the new-seam
// bb_temp_health_desc/bb_temp_health_fill() pair, and bb_temp_register_info()
// against the bb_health_section composer registry (B1-1098, PR-3 of the
// bb_health/bb_response migration chain, epic B1-1054). ADDITIVE AND INERT:
// bb_temp_register_info() now populates ONLY the new bb_health_section
// table -- nothing renders it yet (that cutover is B1-1054 PR-5) -- so
// these tests exercise the fill/registration contract directly, mirroring
// test_bb_diag_meminfo.c's fill-adapter idiom and test_bb_health_section.c's
// registration-fixture idiom.

#include "unity.h"
#include "bb_temp.h"
#include "bb_serialize_json.h"

#include "../../platform/host/bb_temp/bb_temp_test.h"

#include <string.h>
#include <stdio.h>

/* setUp/tearDown are called per-test by the test runner via test_main.c setUp(). */

/* ---- bb_temp_read_soc ---- */

void test_bb_temp_read_soc_default_absent(void)
{
    /* Default host state: present=false → read returns false */
    bb_temp_test_set_soc(false, 0.0f);
    float c = -999.0f;
    bool ok = bb_temp_read_soc(&c);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_FLOAT(-999.0f, c); /* *out untouched on false */
}

void test_bb_temp_read_soc_injected_present(void)
{
    bb_temp_test_set_soc(true, 42.5f);
    float c = 0.0f;
    bool ok = bb_temp_read_soc(&c);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_FLOAT(42.5f, c);
}

void test_bb_temp_read_soc_null_out_returns_false(void)
{
    bb_temp_test_set_soc(true, 30.0f);
    bool ok = bb_temp_read_soc(NULL);
    TEST_ASSERT_FALSE(ok);
}

/* ---- bb_temp_health_fill (bb_health_fill_fn adapter) ---- */

void test_bb_temp_health_fill_rejects_null_dst(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_temp_health_fill(NULL, NULL));
}

void test_bb_temp_health_fill_absent_by_default(void)
{
    bb_temp_test_set_soc(false, 0.0f);

    bb_temp_health_snap_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    bb_health_fill_args_t args = { .ctx = NULL };
    TEST_ASSERT_EQUAL(BB_OK, bb_temp_health_fill(&snap, &args));

    TEST_ASSERT_FALSE(snap.present);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, snap.soc_c);
}

void test_bb_temp_health_fill_present_with_value(void)
{
    bb_temp_test_set_soc(true, 55.3f);

    bb_temp_health_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_temp_health_fill(&snap, NULL));

    TEST_ASSERT_TRUE(snap.present);
    TEST_ASSERT_DOUBLE_WITHIN(0.05, 55.3, snap.soc_c);
}

void test_bb_temp_health_fill_rounds_to_one_decimal(void)
{
    /* 36.789 → rounds to 36.8 */
    bb_temp_test_set_soc(true, 36.789f);

    bb_temp_health_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_temp_health_fill(&snap, NULL));

    TEST_ASSERT_TRUE(snap.present);
    TEST_ASSERT_DOUBLE_WITHIN(0.05, 36.8, snap.soc_c);
}

/* ---- bb_temp_register_info (new bb_health_section seam) ---- */

void test_bb_temp_register_info_registers_into_new_table(void)
{
    bb_health_section_test_reset();
    bb_temp_test_set_soc(false, 0.0f);

    bb_temp_register_info();

    const bb_health_section_t *stored = bb_health_section_test_find("temp");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_STRING("temp", stored->name);
    TEST_ASSERT_EQUAL_PTR(&bb_temp_health_desc, stored->snap_desc);
    TEST_ASSERT_EQUAL_PTR(bb_temp_health_fill, stored->fill);

    bb_health_section_test_reset();
}

void test_bb_temp_register_info_schema_props_present(void)
{
    bb_health_section_test_reset();

    bb_temp_register_info();

    const bb_health_section_t *stored = bb_health_section_test_find("temp");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_NOT_NULL(stored->schema_props);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(stored->schema_props, "\"present\""),
                                 "present key missing from temp schema fragment");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(stored->schema_props, "\"soc_c\""),
                                 "soc_c key missing from temp schema fragment");

    bb_health_section_test_reset();
}

/* ---- structure/presence byte-fidelity vs today's shape ----
 * Asserts field STRUCTURE/presence only (key names + soc_c omitted when
 * absent) -- NOT the exact float digit formatting, which is a render-level
 * concern (B1-1102's f64_shortest flag) applied later at the /api/health
 * cutover (B1-1054 PR-5), not this producer's. */

void test_bb_temp_health_desc_wire_shape_absent_omits_soc_c(void)
{
    bb_temp_test_set_soc(false, 0.0f);

    bb_temp_health_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_temp_health_fill(&snap, NULL));

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_serialize_json_render(&bb_temp_health_desc, &snap, buf, sizeof(buf), &len));

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":false"));
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "\"soc_c\""),
                             "soc_c must be omitted (not null) when absent");
}

void test_bb_temp_health_desc_wire_shape_present_includes_soc_c(void)
{
    bb_temp_test_set_soc(true, 55.3f);

    bb_temp_health_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_temp_health_fill(&snap, NULL));

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_serialize_json_render(&bb_temp_health_desc, &snap, buf, sizeof(buf), &len));

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":true"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"soc_c\""),
                                 "soc_c must be present when sensor reading is present");
}
