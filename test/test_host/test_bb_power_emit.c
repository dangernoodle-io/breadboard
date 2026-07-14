// Tests for bb_power_emit — single JSON builder used by REST responses.
// Verifies: all 5 fields emitted as numbers when >= 0, null when -1.
#include "unity.h"
#include "bb_power.h"
#include "bb_json.h"

#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Helper: emit a snapshot into a fresh object, serialize, parse, return parsed.
// Caller must bb_json_free the returned object.
// ---------------------------------------------------------------------------

static bb_json_t emit_and_parse(const bb_power_snapshot_t *snap)
{
    bb_json_t obj = bb_json_obj_new();
    if (!obj) return NULL;
    bb_power_emit(obj, snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    if (!json) return NULL;
    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    return parsed;
}

// ---------------------------------------------------------------------------
// All fields present (all >= 0)
// ---------------------------------------------------------------------------

void test_bb_power_emit_all_fields_present(void)
{
    bb_power_snapshot_t snap = {
        .vout_mv = 1200,
        .iout_ma = 500,
        .pout_mw = 600,
        .vin_mv  = 5000,
        .temp_c  = 55,
    };

    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    double v = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "vout_mv", &v));
    TEST_ASSERT_EQUAL_INT(1200, (int)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "iout_ma", &v));
    TEST_ASSERT_EQUAL_INT(500, (int)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "pout_mw", &v));
    TEST_ASSERT_EQUAL_INT(600, (int)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "vin_mv", &v));
    TEST_ASSERT_EQUAL_INT(5000, (int)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "temp_c", &v));
    TEST_ASSERT_EQUAL_INT(55, (int)v);

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// Individual field null when -1
// ---------------------------------------------------------------------------

void test_bb_power_emit_vout_null_when_minus_one(void)
{
    bb_power_snapshot_t snap = { .vout_mv = -1, .iout_ma = 100, .pout_mw = -1, .vin_mv = 5000, .temp_c = 40 };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "vout_mv");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

void test_bb_power_emit_iout_null_when_minus_one(void)
{
    bb_power_snapshot_t snap = { .vout_mv = 1200, .iout_ma = -1, .pout_mw = -1, .vin_mv = 5000, .temp_c = 40 };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "iout_ma");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

void test_bb_power_emit_pout_null_when_minus_one(void)
{
    bb_power_snapshot_t snap = { .vout_mv = 1200, .iout_ma = 500, .pout_mw = -1, .vin_mv = 5000, .temp_c = 40 };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "pout_mw");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

void test_bb_power_emit_vin_null_when_minus_one(void)
{
    bb_power_snapshot_t snap = { .vout_mv = 1200, .iout_ma = 500, .pout_mw = 600, .vin_mv = -1, .temp_c = 40 };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "vin_mv");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

void test_bb_power_emit_temp_null_when_minus_one(void)
{
    bb_power_snapshot_t snap = { .vout_mv = 1200, .iout_ma = 500, .pout_mw = 600, .vin_mv = 5000, .temp_c = -1 };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "temp_c");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// All -1 → all null
// ---------------------------------------------------------------------------

void test_bb_power_emit_all_null_when_all_minus_one(void)
{
    bb_power_snapshot_t snap = { .vout_mv = -1, .iout_ma = -1, .pout_mw = -1, .vin_mv = -1, .temp_c = -1 };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    const char *fields[] = { "vout_mv", "iout_ma", "pout_mw", "vin_mv", "temp_c" };
    for (int i = 0; i < 5; i++) {
        bb_json_t item = bb_json_obj_get_item(parsed, fields[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(item, fields[i]);
        TEST_ASSERT_TRUE_MESSAGE(bb_json_item_is_null(item), fields[i]);
    }

    bb_json_free(parsed);
}
