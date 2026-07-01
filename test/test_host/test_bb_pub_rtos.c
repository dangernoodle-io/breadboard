// Tests for bb_pub_rtos: sample fn publishes FreeRTOS stack high-water-marks.
// Uses the host stub which returns deterministic values.
#include "unity.h"
#include "bb_pub_rtos.h"
#include "bb_pub.h"
#include "bb_nv.h"
#include "bb_task_registry.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Fake capturing sink
// ---------------------------------------------------------------------------

#define CAPTURE_CAP 16

typedef struct {
    char topic[192];
    char payload[512];
} rtos_capture_entry_t;

static rtos_capture_entry_t s_captured[CAPTURE_CAP];
static int                  s_capture_count;

static bb_err_t rtos_capture_publish(void *ctx, const char *topic,
                                     const char *payload, int len, bool retain)
{
    (void)ctx;
    (void)len;
    (void)retain;
    if (s_capture_count >= CAPTURE_CAP) return BB_ERR_NO_SPACE;
    rtos_capture_entry_t *e = &s_captured[s_capture_count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    e->topic[sizeof(e->topic) - 1] = '\0';
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    e->payload[sizeof(e->payload) - 1] = '\0';
    return BB_OK;
}

static void rtos_capture_reset(void)
{
    memset(s_captured, 0, sizeof(s_captured));
    s_capture_count = 0;
}

static void setup(void)
{
    bb_pub_test_reset();
    bb_task_registry_test_reset();
    rtos_capture_reset();
    bb_nv_config_set_hostname("testhost");

    // Mirrors production: bb_pub's own worker task always self-registers
    // into bb_task_registry under "bb_pub" (see the s_named[] comment in
    // bb_pub_rtos.c) before this sample runs. The host stub no longer
    // hardcodes "stack_bb_pub" (B1-450) — it is registry-sourced, so tests
    // seed it here the same way production populates it.
    bb_task_registry_test_seed("bb_pub", 2048, false, NULL);

    bb_pub_sink_t sink = { .publish = rtos_capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_rtos_register();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_rtos_always_publishes(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_rtos_topic_is_correct(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/rtos", s_captured[0].topic);
}

void test_bb_pub_rtos_has_min_free_stack(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"min_free_stack\""));
}

void test_bb_pub_rtos_has_min_free_stack_task(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"min_free_stack_task\""));
}

void test_bb_pub_rtos_has_task_count(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"task_count\""));
}

void test_bb_pub_rtos_task_count_is_nonzero(void)
{
    setup();
    bb_pub_tick_once();
    // host stub returns task_count=8
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"task_count\":8"));
}

void test_bb_pub_rtos_min_free_stack_is_smallest(void)
{
    setup();
    bb_pub_tick_once();
    // host stub: min_free_stack=2048, stack_bb_pub=2048, stack_httpd=3072 etc.
    // min must equal stack_bb_pub (the smallest stub value).
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"min_free_stack\":2048"));
}

void test_bb_pub_rtos_min_free_stack_task_is_bb_pub(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"min_free_stack_task\":\"bb_pub\""));
}

void test_bb_pub_rtos_has_stack_bb_pub(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_bb_pub\""));
}

void test_bb_pub_rtos_has_stack_httpd(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_httpd\""));
}

void test_bb_pub_rtos_has_stack_mqtt(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_mqtt\""));
}

void test_bb_pub_rtos_has_stack_ipc0(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_ipc0\""));
}

void test_bb_pub_rtos_has_stack_ipc1(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_ipc1\""));
}

void test_bb_pub_rtos_has_stack_main(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_main\""));
}

void test_bb_pub_rtos_payload_has_uptime_ms_field(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"uptime_ms\""));
}

// ---------------------------------------------------------------------------
// bb_task_registry-driven fields (B1-445) — additive, host-testable via
// bb_task_registry_test_seed (no real TaskHandle_t on host).
// ---------------------------------------------------------------------------

void test_bb_pub_rtos_emits_one_stack_field_per_registered_entry(void)
{
    setup();
    bb_task_registry_test_seed("my_task", 7168, false, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_my_task\":7168"));
}

void test_bb_pub_rtos_emits_multiple_registered_entries(void)
{
    setup();
    bb_task_registry_test_seed("task_one", 1024, false, NULL);
    bb_task_registry_test_seed("task_two", 2048, true, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_task_one\":1024"));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_task_two\":2048"));
}

void test_bb_pub_rtos_no_registered_entries_still_publishes(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // "No registered entries" means no ADDITIONAL app-level entries beyond
    // bb_pub's own self-registration, which setup() seeds unconditionally
    // (mirrors production — bb_pub always registers itself). stack_bb_pub is
    // registry-sourced (B1-450), not a hardcoded literal.
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"stack_bb_pub\""));
}

// B1-450: stack_bb_pub is registry-sourced only (removed from s_named[] on
// ESP-IDF, and never hardcoded on host — see the comments above s_named[]
// and in the host rtos_sample() stub in bb_pub_rtos.c). setup() seeds
// "bb_pub" into bb_task_registry the same way bb_pub's worker self-registers
// in production, so this test reproduces the defect class this fix
// addresses: without the fix, a hardcoded literal PLUS the registry-driven
// foreach would double-emit the key for a registered "bb_pub" entry. Assert
// the field appears, and appears exactly once.
void test_bb_pub_rtos_stack_bb_pub_appears_exactly_once(void)
{
    setup();
    bb_pub_tick_once();
    const char *first = strstr(s_captured[0].payload, "\"stack_bb_pub\"");
    TEST_ASSERT_NOT_NULL(first);
    const char *second = strstr(first + 1, "\"stack_bb_pub\"");
    TEST_ASSERT_NULL(second);
}

void test_bb_pub_rtos_benign_task_filter(void)
{
    // Benign system tasks are excluded from the min_free_stack headline.
    TEST_ASSERT_TRUE(bb_pub_rtos_is_benign_task("ipc0"));
    TEST_ASSERT_TRUE(bb_pub_rtos_is_benign_task("ipc1"));
    TEST_ASSERT_TRUE(bb_pub_rtos_is_benign_task("IDLE"));
    TEST_ASSERT_TRUE(bb_pub_rtos_is_benign_task("IDLE0"));
    TEST_ASSERT_TRUE(bb_pub_rtos_is_benign_task("esp_timer"));
    TEST_ASSERT_TRUE(bb_pub_rtos_is_benign_task("Tmr Svc"));
    // App tasks are NOT benign — they drive the at-risk metric.
    TEST_ASSERT_FALSE(bb_pub_rtos_is_benign_task("bb_pub"));
    TEST_ASSERT_FALSE(bb_pub_rtos_is_benign_task("httpd"));
    TEST_ASSERT_FALSE(bb_pub_rtos_is_benign_task("mining"));
    TEST_ASSERT_FALSE(bb_pub_rtos_is_benign_task(NULL));
}
