// Tests for bb_pub_telemetry section:
// - GET has all required fields
// - GET status reflects actual bb_pub state
#include "unity.h"
#include "bb_pub.h"
#include "bb_pub_telemetry.h"
#include "bb_nv.h"
#include "bb_json.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// Test hook declarations from host twin.
void bb_pub_telemetry_section_get_for_test(bb_json_t section, void *ctx);
bb_err_t bb_pub_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);

// ---------------------------------------------------------------------------
// Sample + sink helpers
// ---------------------------------------------------------------------------

static bool sample_temp(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "value_c", 42.0);
    return true;
}

static bb_err_t failing_sink_fn(void *ctx, const char *topic,
                                 const char *payload, int len)
{
    (void)ctx; (void)topic; (void)payload; (void)len;
    return BB_ERR_INVALID_STATE;
}

static int s_cap_count;
static bb_err_t cap_sink_fn(void *ctx, const char *topic,
                              const char *payload, int len)
{
    (void)ctx; (void)topic; (void)payload; (void)len;
    s_cap_count++;
    return BB_OK;
}

static void reset_all(void)
{
    bb_pub_telemetry_reset_for_test();
    bb_nv_host_str_store_reset();
    bb_nv_config_set_hostname("testhost");
    s_cap_count = 0;
}

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static cJSON *run_get(void)
{
    bb_json_t section = bb_json_obj_new();
    bb_pub_telemetry_section_get_for_test(section, NULL);
    char *s = bb_json_serialize(section);
    bb_json_free(section);
    if (!s) return NULL;
    cJSON *parsed = cJSON_Parse(s);
    bb_json_free_str(s);
    return parsed;
}

// ---------------------------------------------------------------------------
// Required fields present
// ---------------------------------------------------------------------------

void test_bb_pub_telemetry_get_has_interval_ms(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "GET did not emit valid JSON");
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "interval_ms");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsNumber(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_has_topic_prefix(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "topic_prefix");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsString(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_has_source_count(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "source_count");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsNumber(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_has_sink_count(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "sink_count");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsNumber(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_has_published_ever(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "published_ever");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsBool(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_has_last_publish_ok(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "last_publish_ok");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsBool(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_has_last_publish_age_ms(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "last_publish_age_ms");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsNumber(f));
    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// Status reflects actual bb_pub state
// ---------------------------------------------------------------------------

void test_bb_pub_telemetry_get_counts_reflect_registered(void)
{
    reset_all();
    bb_pub_sink_t s = { .publish = cap_sink_fn, .ctx = NULL };
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temp, NULL);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);

    cJSON *sc = cJSON_GetObjectItemCaseSensitive(body, "source_count");
    cJSON *sk = cJSON_GetObjectItemCaseSensitive(body, "sink_count");
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetNumberValue(sc));
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetNumberValue(sk));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_published_ever_false_before_tick(void)
{
    reset_all();
    bb_pub_sink_t s = { .publish = cap_sink_fn, .ctx = NULL };
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temp, NULL);

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *pe = cJSON_GetObjectItemCaseSensitive(body, "published_ever");
    TEST_ASSERT_FALSE(cJSON_IsTrue(pe));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_published_ever_true_after_tick(void)
{
    reset_all();
    bb_pub_sink_t s = { .publish = cap_sink_fn, .ctx = NULL };
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temp, NULL);
    bb_pub_tick_once();

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *pe = cJSON_GetObjectItemCaseSensitive(body, "published_ever");
    TEST_ASSERT_TRUE(cJSON_IsTrue(pe));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_last_publish_age_minus1_when_never(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *age = cJSON_GetObjectItemCaseSensitive(body, "last_publish_age_ms");
    TEST_ASSERT_EQUAL_INT(-1, (int)cJSON_GetNumberValue(age));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_last_publish_age_nonneg_after_tick(void)
{
    reset_all();
    bb_pub_sink_t s = { .publish = cap_sink_fn, .ctx = NULL };
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temp, NULL);
    bb_pub_tick_once();

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *age = cJSON_GetObjectItemCaseSensitive(body, "last_publish_age_ms");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, (int)cJSON_GetNumberValue(age));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_last_publish_ok_false_with_failing_sink(void)
{
    reset_all();
    bb_pub_sink_t bad = { .publish = failing_sink_fn, .ctx = NULL };
    bb_pub_add_sink(&bad);
    bb_pub_register_source("temp", sample_temp, NULL);
    bb_pub_tick_once();

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(body, "last_publish_ok");
    TEST_ASSERT_FALSE(cJSON_IsTrue(ok));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_last_publish_ok_true_with_good_sink(void)
{
    reset_all();
    bb_pub_sink_t s = { .publish = cap_sink_fn, .ctx = NULL };
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temp, NULL);
    bb_pub_tick_once();

    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(body, "last_publish_ok");
    TEST_ASSERT_TRUE(cJSON_IsTrue(ok));
    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// GET reports interval_ms and enabled
// ---------------------------------------------------------------------------

void test_bb_pub_telemetry_get_has_enabled(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsBool(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_interval_ms_reflects_default(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "interval_ms");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(CONFIG_BB_PUB_INTERVAL_MS, (int)cJSON_GetNumberValue(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_get_enabled_true_by_default(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    TEST_ASSERT_TRUE(cJSON_IsTrue(f));
    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// PATCH helpers
// ---------------------------------------------------------------------------

static bb_err_t run_patch_json(const char *json_str)
{
    bb_json_t patch = bb_json_parse(json_str, 0);
    if (!patch) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_pub_telemetry_section_patch_for_test(patch, NULL);
    bb_json_free(patch);
    return err;
}

// ---------------------------------------------------------------------------
// PATCH sets interval and applies
// ---------------------------------------------------------------------------

void test_bb_pub_telemetry_patch_interval_ms_updates_getter(void)
{
    reset_all();
    bb_err_t err = run_patch_json("{\"interval_ms\":5000}");
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_UINT32(5000, bb_pub_get_interval_ms());
}

void test_bb_pub_telemetry_patch_interval_ms_reflected_in_get(void)
{
    reset_all();
    run_patch_json("{\"interval_ms\":5000}");
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "interval_ms");
    TEST_ASSERT_EQUAL_INT(5000, (int)cJSON_GetNumberValue(f));
    cJSON_Delete(body);
}

void test_bb_pub_telemetry_patch_interval_ms_zero_rejected(void)
{
    reset_all();
    bb_err_t err = run_patch_json("{\"interval_ms\":0}");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    // getter unchanged
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_telemetry_patch_interval_ms_below_min_rejected(void)
{
    reset_all();
    bb_err_t err = run_patch_json("{\"interval_ms\":500}");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_pub_telemetry_patch_interval_ms_above_max_rejected(void)
{
    reset_all();
    bb_err_t err = run_patch_json("{\"interval_ms\":9999999}");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// ---------------------------------------------------------------------------
// PATCH sets enabled and applies
// ---------------------------------------------------------------------------

void test_bb_pub_telemetry_patch_enabled_false_persists(void)
{
    reset_all();
    bb_err_t err = run_patch_json("{\"enabled\":false}");
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(bb_pub_is_enabled());
}

void test_bb_pub_telemetry_patch_enabled_true_persists(void)
{
    reset_all();
    bb_pub_set_enabled(false);
    bb_err_t err = run_patch_json("{\"enabled\":true}");
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
}

void test_bb_pub_telemetry_patch_enabled_reflected_in_get(void)
{
    reset_all();
    run_patch_json("{\"enabled\":false}");
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    TEST_ASSERT_FALSE(cJSON_IsTrue(f));
    cJSON_Delete(body);
}

// ---------------------------------------------------------------------------
// PATCH partial updates — only present fields are touched
// ---------------------------------------------------------------------------

void test_bb_pub_telemetry_patch_partial_only_changes_present_fields(void)
{
    reset_all();
    // Set known state.
    bb_pub_set_interval_ms(2000);
    bb_pub_set_enabled(true);

    // Patch only enabled.
    bb_err_t err = run_patch_json("{\"enabled\":false}");
    TEST_ASSERT_EQUAL(BB_OK, err);
    // interval unchanged.
    TEST_ASSERT_EQUAL_UINT32(2000, bb_pub_get_interval_ms());
    // enabled changed.
    TEST_ASSERT_FALSE(bb_pub_is_enabled());
}

// ---------------------------------------------------------------------------
// B1-398: publisher.available field
// ---------------------------------------------------------------------------

// GET publisher section includes "available" field.
void test_bb_pub_telemetry_get_has_available(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "GET did not emit valid JSON");
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "available");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "missing 'available' field");
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsBool(f), "'available' must be a boolean");
    cJSON_Delete(body);
}

// available is false by default (publisher not started on host builds).
void test_bb_pub_telemetry_get_available_false_by_default(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "available");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_FALSE_MESSAGE(cJSON_IsTrue(f),
                               "available must be false before mark_started");
    cJSON_Delete(body);
}

// available becomes true after bb_pub_mark_started() is called.
void test_bb_pub_telemetry_get_available_true_after_mark_started(void)
{
    reset_all();
    bb_pub_mark_started();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "available");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsTrue(f),
                              "available must be true after mark_started");
    cJSON_Delete(body);
}

// PATCH enabled=true when publisher is not available still returns BB_OK
// (the NVS write is harmless; the route handler generates honest response).
void test_bb_pub_telemetry_patch_enabled_on_unavailable_publisher_returns_ok(void)
{
    reset_all();
    // Publisher not started (s_started=false after reset).
    bb_err_t err = run_patch_json("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_MESSAGE(BB_OK, err,
        "section patch must return BB_OK even when publisher is unavailable");
    // The NVS-persisted flag still reflects the requested value.
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
}

// PATCH enabled=true when publisher IS available also returns BB_OK.
void test_bb_pub_telemetry_patch_enabled_on_available_publisher_returns_ok(void)
{
    reset_all();
    bb_pub_mark_started();
    bb_err_t err = run_patch_json("{\"enabled\":true}");
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
}

// bb_pub_get_status().available is false by default (no mark_started call).
void test_bb_pub_get_status_available_false_by_default(void)
{
    reset_all();
    bb_pub_status_t st = {0};
    bb_pub_get_status(&st);
    TEST_ASSERT_FALSE_MESSAGE(st.available,
        "available must be false before mark_started");
}

// bb_pub_get_status().available is true after mark_started.
void test_bb_pub_get_status_available_true_after_mark_started(void)
{
    reset_all();
    bb_pub_mark_started();
    bb_pub_status_t st = {0};
    bb_pub_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.available,
        "available must be true after mark_started");
}

// mark_started is cleared by test_reset so tests are isolated.
void test_bb_pub_get_status_available_reset_clears_it(void)
{
    reset_all();
    bb_pub_mark_started();
    // Reset clears it.
    bb_pub_telemetry_reset_for_test();
    bb_pub_status_t st = {0};
    bb_pub_get_status(&st);
    TEST_ASSERT_FALSE_MESSAGE(st.available,
        "available must be false after test_reset");
}
