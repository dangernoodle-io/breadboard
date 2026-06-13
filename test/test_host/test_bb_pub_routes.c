// Tests for GET /api/pub route: JSON fields, status reflection.
#include "unity.h"
#include "bb_pub.h"
#include "bb_pub_routes.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "cJSON.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// Test hook declarations from host twin.
bb_err_t bb_pub_routes_get_handler_for_test(bb_http_request_t *req);

// ---------------------------------------------------------------------------
// Sample + sink helpers (reuse from test_bb_pub pattern)
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

static int     s_cap_count;
static bb_err_t cap_sink_fn(void *ctx, const char *topic,
                              const char *payload, int len)
{
    (void)ctx; (void)topic; (void)payload; (void)len;
    s_cap_count++;
    return BB_OK;
}

static void reset_all(void)
{
    bb_pub_routes_reset_for_test();
    bb_nv_host_str_store_reset();
    bb_nv_config_set_hostname("testhost");
    s_cap_count = 0;
}

// ---------------------------------------------------------------------------
// Helper: invoke GET handler, parse body
// ---------------------------------------------------------------------------

static cJSON *run_get(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_pub_routes_get_handler_for_test(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    cJSON *parsed = NULL;
    if (cap.body) parsed = cJSON_Parse(cap.body);
    bb_http_host_capture_free(&cap);
    return parsed;
}

// ---------------------------------------------------------------------------
// Tests — required fields present
// ---------------------------------------------------------------------------

void test_bb_pub_routes_get_has_interval_ms(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "GET did not emit valid JSON");
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "interval_ms");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsNumber(f));
    cJSON_Delete(body);
}

void test_bb_pub_routes_get_has_topic_prefix(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL_MESSAGE(body, "GET did not emit valid JSON");
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "topic_prefix");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsString(f));
    cJSON_Delete(body);
}

void test_bb_pub_routes_get_has_source_count(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "source_count");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsNumber(f));
    cJSON_Delete(body);
}

void test_bb_pub_routes_get_has_sink_count(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "sink_count");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsNumber(f));
    cJSON_Delete(body);
}

void test_bb_pub_routes_get_has_published_ever(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "published_ever");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsBool(f));
    cJSON_Delete(body);
}

void test_bb_pub_routes_get_has_last_publish_ok(void)
{
    reset_all();
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *f = cJSON_GetObjectItemCaseSensitive(body, "last_publish_ok");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(cJSON_IsBool(f));
    cJSON_Delete(body);
}

void test_bb_pub_routes_get_has_last_publish_age_ms(void)
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
// Tests — status reflects actual bb_pub state
// ---------------------------------------------------------------------------

void test_bb_pub_routes_get_counts_reflect_registered(void)
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

void test_bb_pub_routes_get_published_ever_false_before_tick(void)
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

void test_bb_pub_routes_get_published_ever_true_after_tick(void)
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

void test_bb_pub_routes_get_last_publish_age_minus1_when_never(void)
{
    reset_all();
    // No sink, no tick.
    cJSON *body = run_get();
    TEST_ASSERT_NOT_NULL(body);
    cJSON *age = cJSON_GetObjectItemCaseSensitive(body, "last_publish_age_ms");
    TEST_ASSERT_EQUAL_INT(-1, (int)cJSON_GetNumberValue(age));
    cJSON_Delete(body);
}

void test_bb_pub_routes_get_last_publish_age_nonneg_after_tick(void)
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

void test_bb_pub_routes_get_last_publish_ok_false_with_failing_sink(void)
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

void test_bb_pub_routes_get_last_publish_ok_true_with_good_sink(void)
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
