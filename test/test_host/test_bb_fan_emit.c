// Tests for bb_fan_emit — single JSON builder used by REST responses.
// Verifies: all 4 fields emitted as numbers when valid, null when unavailable.
#include "unity.h"
#include "bb_fan.h"
#include "bb_json.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Helper: emit a snapshot into a fresh object, serialize, parse, return parsed.
// Caller must bb_json_free the returned object.
// ---------------------------------------------------------------------------

#ifndef CONFIG_BB_FAN_AUTOFAN
static bb_json_t emit_and_parse(const bb_fan_snapshot_t *snap)
{
    bb_json_t obj = bb_json_obj_new();
    if (!obj) return NULL;
    bb_fan_emit(obj, snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    if (!json) return NULL;
    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    return parsed;
}
#else
static bb_json_t emit_and_parse_with_tel(const bb_fan_snapshot_t *snap,
                                          const bb_fan_autofan_telemetry_t *tel)
{
    bb_json_t obj = bb_json_obj_new();
    if (!obj) return NULL;
    bb_fan_emit(obj, snap, tel);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    if (!json) return NULL;
    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    return parsed;
}

static bb_json_t emit_and_parse(const bb_fan_snapshot_t *snap)
{
    bb_fan_autofan_telemetry_t tel = {
        .die_ema_c     = -1.0f,
        .aux_ema_c     = -1.0f,
        .pid_input_c   = -1.0f,
        .pid_input_src = "",
    };
    return emit_and_parse_with_tel(snap, &tel);
}
#endif

// ---------------------------------------------------------------------------
// All fields present
// ---------------------------------------------------------------------------

void test_bb_fan_emit_all_fields_present(void)
{
    bb_fan_snapshot_t snap = {
        .rpm      = 2400,
        .duty_pct = 75,
        .die_c    = 45.5f,
        .board_c  = 32.0f,
    };

    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    double v = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "rpm", &v));
    TEST_ASSERT_EQUAL_INT(2400, (int)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "duty_pct", &v));
    TEST_ASSERT_EQUAL_INT(75, (int)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "die_c", &v));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.5f, (float)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "board_c", &v));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 32.0f, (float)v);

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// Individual field null when unavailable
// ---------------------------------------------------------------------------

void test_bb_fan_emit_rpm_null_when_minus_one(void)
{
    bb_fan_snapshot_t snap = { .rpm = -1, .duty_pct = 75, .die_c = 45.5f, .board_c = 32.0f };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "rpm");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

void test_bb_fan_emit_duty_null_when_minus_one(void)
{
    bb_fan_snapshot_t snap = { .rpm = 2400, .duty_pct = -1, .die_c = 45.5f, .board_c = 32.0f };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "duty_pct");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

void test_bb_fan_emit_die_c_null_when_nan(void)
{
    bb_fan_snapshot_t snap = { .rpm = 2400, .duty_pct = 75, .die_c = NAN, .board_c = 32.0f };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "die_c");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

void test_bb_fan_emit_board_c_null_when_nan(void)
{
    bb_fan_snapshot_t snap = { .rpm = 2400, .duty_pct = 75, .die_c = 45.5f, .board_c = NAN };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "board_c");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// All unavailable → all null
// ---------------------------------------------------------------------------

void test_bb_fan_emit_all_null_when_unavailable(void)
{
    bb_fan_snapshot_t snap = { .rpm = -1, .duty_pct = -1, .die_c = NAN, .board_c = NAN };
    bb_json_t parsed = emit_and_parse(&snap);
    TEST_ASSERT_NOT_NULL(parsed);

    const char *fields[] = { "rpm", "duty_pct", "die_c", "board_c" };
    for (int i = 0; i < 4; i++) {
        bb_json_t item = bb_json_obj_get_item(parsed, fields[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(item, fields[i]);
        TEST_ASSERT_TRUE_MESSAGE(bb_json_item_is_null(item), fields[i]);
    }

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// Autofan tests (only compiled when CONFIG_BB_FAN_AUTOFAN is defined)
// ---------------------------------------------------------------------------

#ifdef CONFIG_BB_FAN_AUTOFAN

void test_bb_fan_emit_autofan_fields_present(void)
{
    bb_fan_snapshot_t snap = { .rpm = 2400, .duty_pct = 75, .die_c = 45.5f, .board_c = 32.0f };
    bb_fan_autofan_telemetry_t tel = {
        .die_ema_c     = 44.0f,
        .aux_ema_c     = 55.0f,
        .pid_input_c   = 44.0f,
        .pid_input_src = "die",
    };

    bb_json_t parsed = emit_and_parse_with_tel(&snap, &tel);
    TEST_ASSERT_NOT_NULL(parsed);

    double v = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "die_ema_c", &v));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 44.0f, (float)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "vr_ema_c", &v));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 55.0f, (float)v);

    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "pid_input_c", &v));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 44.0f, (float)v);

    bb_json_free(parsed);
}

void test_bb_fan_emit_autofan_aux_mapped_to_vr(void)
{
    bb_fan_snapshot_t snap = { .rpm = 2400, .duty_pct = 75, .die_c = 45.5f, .board_c = 32.0f };
    bb_fan_autofan_telemetry_t tel = {
        .die_ema_c     = 44.0f,
        .aux_ema_c     = 55.0f,
        .pid_input_c   = 55.0f,
        .pid_input_src = "aux",
    };

    bb_json_t parsed = emit_and_parse_with_tel(&snap, &tel);
    TEST_ASSERT_NOT_NULL(parsed);

    char src[16] = {0};
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "pid_input_src", src, sizeof(src)));
    TEST_ASSERT_EQUAL_STRING("vr", src);

    bb_json_free(parsed);
}

void test_bb_fan_emit_autofan_die_ema_null_when_negative(void)
{
    bb_fan_snapshot_t snap = { .rpm = 2400, .duty_pct = 75, .die_c = 45.5f, .board_c = 32.0f };
    bb_fan_autofan_telemetry_t tel = {
        .die_ema_c     = -1.0f,
        .aux_ema_c     = 55.0f,
        .pid_input_c   = 55.0f,
        .pid_input_src = "aux",
    };

    bb_json_t parsed = emit_and_parse_with_tel(&snap, &tel);
    TEST_ASSERT_NOT_NULL(parsed);

    bb_json_t item = bb_json_obj_get_item(parsed, "die_ema_c");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_null(item));

    bb_json_free(parsed);
}

#endif /* CONFIG_BB_FAN_AUTOFAN */
