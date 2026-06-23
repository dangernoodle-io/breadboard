// Parity smoke test: bb_pub_register_source and bb_pub_register_source_ex
// produce the same publishing behavior when no tags / no subscribe filter.
// Also: bb_pub_power and bb_power_emit share the same builder (B1-352).
// Also: bb_pub_fan and bb_fan_emit share the same builder (B1-352).
#include "unity.h"
#include "bb_pub.h"
#include "bb_pub_power.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include "bb_pub_fan.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include "bb_json.h"
#include "bb_nv.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Sample helpers
// ---------------------------------------------------------------------------

static bool sample_a(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "a", 42.0);
    return true;
}

// ---------------------------------------------------------------------------
// Capture helpers
// ---------------------------------------------------------------------------

static int s_publish_count = 0;

static bb_err_t count_publish(void *ctx, const char *topic, const char *payload, int len)
{
    (void)ctx;
    (void)topic;
    (void)payload;
    (void)len;
    s_publish_count++;
    return BB_OK;
}

static void setup_test(void)
{
    // Global setUp already calls bb_pub_test_reset().
    s_publish_count = 0;
    bb_nv_config_set_hostname("parityhost");
}

// ---------------------------------------------------------------------------
// Parity tests
// ---------------------------------------------------------------------------

void test_bb_pub_parity_register_source_ex_null_tags_same_behavior(void)
{
    setup_test();
    bb_pub_sink_t sink = { .publish = count_publish };
    bb_pub_set_sink(&sink);

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source_ex("a", sample_a, NULL, NULL, 0));
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL(1, s_publish_count);
}

void test_bb_pub_parity_source_and_source_ex_both_publish(void)
{
    setup_test();
    bb_pub_sink_t sink = { .publish = count_publish };
    bb_pub_set_sink(&sink);

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source("a", sample_a, NULL));
    const char *tags[] = { "x" };
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source_ex("b", sample_a, NULL, tags, 1));
    bb_pub_tick_once();
    // Both sources publish: count should be 2 (one per source, one sink).
    TEST_ASSERT_EQUAL(2, s_publish_count);
}

void test_bb_pub_parity_source_info_ex_null_tags_returns_zero(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source("a", sample_a, NULL));
    int ntags = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_source_info_ex(0, NULL, NULL, NULL, NULL, NULL, NULL, &ntags));
    TEST_ASSERT_EQUAL(0, ntags);
}

void test_bb_pub_parity_subscription_predicate_null_ctx_pass_all(void)
{
    // When ctx is NULL, bb_pub_subscription_predicate treats sub as NULL → pass all.
    // (NULL bb_pub_subscription_t pointer → match all)
    TEST_ASSERT_TRUE(bb_pub_subscription_predicate("power", NULL, 0, NULL));
}

// ---------------------------------------------------------------------------
// B1-352 parity: bb_pub_power and bb_power_emit use the same builder.
// ---------------------------------------------------------------------------

static char s_parity_payload[512];
static int  s_parity_count;

static bb_err_t parity_capture(void *ctx, const char *topic,
                                const char *payload, int len)
{
    (void)ctx;
    (void)topic;
    (void)len;
    s_parity_count++;
    strncpy(s_parity_payload, payload, sizeof(s_parity_payload) - 1);
    return BB_OK;
}

typedef struct { int vout_mv, iout_ma, vin_mv, temp_c; } parity_fake_t;
static parity_fake_t g_parity_fake;
static int parity_vout(void *s) { return ((parity_fake_t *)s)->vout_mv; }
static int parity_iout(void *s) { return ((parity_fake_t *)s)->iout_ma; }
static int parity_vin (void *s) { return ((parity_fake_t *)s)->vin_mv; }
static int parity_temp(void *s) { return ((parity_fake_t *)s)->temp_c; }
static const bb_power_driver_t parity_drv = {
    .read_vout_mv = parity_vout,
    .read_iout_ma = parity_iout,
    .read_vin_mv  = parity_vin,
    .read_temp_c  = parity_temp,
    .name         = "parity_drv",
};

void test_bb_pub_power_parity_emit_matches_rest_core_fields(void)
{
    // Build a fake power handle with known values.
    g_parity_fake.vout_mv = 1100;
    g_parity_fake.iout_ma = 3000;
    g_parity_fake.vin_mv  = 4800;
    g_parity_fake.temp_c  = 62;

    bb_pub_test_reset();
    bb_power_test_reset();
    bb_nv_config_set_hostname("parityhost2");

    bb_power_handle_t ph = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_handle_create(&parity_drv, &g_parity_fake, &ph));
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    // --- Direct bb_power_emit path ---
    bb_power_snapshot_t snap;
    bb_power_snapshot(ph, &snap);

    bb_json_t direct_obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(direct_obj);
    bb_power_emit(direct_obj, &snap);
    char *direct_json = bb_json_serialize(direct_obj);
    bb_json_free(direct_obj);
    TEST_ASSERT_NOT_NULL(direct_json);
    bb_json_t direct = bb_json_parse(direct_json, strlen(direct_json));
    bb_json_free_str(direct_json);
    TEST_ASSERT_NOT_NULL(direct);

    // --- bb_pub_power path (same builder under the hood) ---
    memset(s_parity_payload, 0, sizeof(s_parity_payload));
    s_parity_count = 0;
    bb_pub_sink_t sink = { .publish = parity_capture, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_power_register();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_parity_count);

    bb_json_t pub = bb_json_parse(s_parity_payload, strlen(s_parity_payload));
    TEST_ASSERT_NOT_NULL(pub);

    // Both paths must emit the same 5 field values.
    const char *fields[] = { "vout_mv", "iout_ma", "pout_mw", "vin_mv", "temp_c" };
    for (int i = 0; i < 5; i++) {
        double dv = 0.0, pv = 0.0;
        bool d_ok = bb_json_obj_get_number(direct, fields[i], &dv);
        bool p_ok = bb_json_obj_get_number(pub,    fields[i], &pv);
        TEST_ASSERT_EQUAL_MESSAGE(d_ok, p_ok, fields[i]);
        if (d_ok) {
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)dv, (int)pv, fields[i]);
        }
    }

    bb_json_free(direct);
    bb_json_free(pub);
    bb_power_test_reset();
}

// ---------------------------------------------------------------------------
// B1-352 parity: bb_pub_fan and bb_fan_emit use the same builder.
// ---------------------------------------------------------------------------

typedef struct { int rpm, duty_pct; float die_c, board_c; } fan_parity_fake_t;
static fan_parity_fake_t g_fan_parity_fake;
static int fan_parity_rpm     (void *s) { return ((fan_parity_fake_t *)s)->rpm; }
static int fan_parity_duty    (void *s) { return ((fan_parity_fake_t *)s)->duty_pct; }
static bb_err_t fan_parity_die(void *s, float *out) { *out = ((fan_parity_fake_t *)s)->die_c; return BB_OK; }
static bb_err_t fan_parity_board(void *s, float *out) { *out = ((fan_parity_fake_t *)s)->board_c; return BB_OK; }
static const bb_fan_driver_t fan_parity_drv = {
    .read_rpm          = fan_parity_rpm,
    .get_duty_pct      = fan_parity_duty,
    .read_die_temp_c   = fan_parity_die,
    .read_board_temp_c = fan_parity_board,
    .name              = "fan_parity_drv",
};

static char s_fan_parity_payload[512];
static int  s_fan_parity_count;

static bb_err_t fan_parity_capture(void *ctx, const char *topic,
                                    const char *payload, int len)
{
    (void)ctx;
    (void)topic;
    (void)len;
    s_fan_parity_count++;
    strncpy(s_fan_parity_payload, payload, sizeof(s_fan_parity_payload) - 1);
    return BB_OK;
}

void test_bb_pub_fan_parity_emit_matches_pub_source(void)
{
    g_fan_parity_fake.rpm      = 2400;
    g_fan_parity_fake.duty_pct = 75;
    g_fan_parity_fake.die_c    = 45.5f;
    g_fan_parity_fake.board_c  = 32.0f;

    bb_pub_test_reset();
    bb_fan_test_reset();
    bb_nv_config_set_hostname("fanparityhost");

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&fan_parity_drv, &g_fan_parity_fake, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    // --- Direct bb_fan_emit path ---
    bb_fan_snapshot_t snap;
    bb_fan_snapshot(fh, &snap);

    bb_json_t direct_obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(direct_obj);
#ifndef CONFIG_BB_FAN_AUTOFAN
    bb_fan_emit(direct_obj, &snap);
#else
    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(fh, &tel);
    bb_fan_emit(direct_obj, &snap, &tel);
#endif
    char *direct_json = bb_json_serialize(direct_obj);
    bb_json_free(direct_obj);
    TEST_ASSERT_NOT_NULL(direct_json);
    bb_json_t direct = bb_json_parse(direct_json, strlen(direct_json));
    bb_json_free_str(direct_json);
    TEST_ASSERT_NOT_NULL(direct);

    // --- bb_pub_fan path (same builder under the hood) ---
    memset(s_fan_parity_payload, 0, sizeof(s_fan_parity_payload));
    s_fan_parity_count = 0;
    bb_pub_sink_t sink = { .publish = fan_parity_capture, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_fan_register();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_fan_parity_count);

    bb_json_t pub = bb_json_parse(s_fan_parity_payload, strlen(s_fan_parity_payload));
    TEST_ASSERT_NOT_NULL(pub);

    // Both paths must emit the same 4 core field values.
    const char *fields[] = { "rpm", "duty_pct", "die_c", "board_c" };
    for (int i = 0; i < 4; i++) {
        double dv = 0.0, pv = 0.0;
        bool d_ok = bb_json_obj_get_number(direct, fields[i], &dv);
        bool p_ok = bb_json_obj_get_number(pub,    fields[i], &pv);
        TEST_ASSERT_EQUAL_MESSAGE(d_ok, p_ok, fields[i]);
        if (d_ok) {
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, (float)dv, (float)pv, fields[i]);
        }
    }

    bb_json_free(direct);
    bb_json_free(pub);
    bb_fan_test_reset();
}
