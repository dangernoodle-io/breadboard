// Tests for bb_pub_mem: sample fn always publishes memory fields; psram omitted when no PSRAM.
#include "unity.h"
#include "bb_pub_mem.h"
#include "bb_pub.h"
#include "bb_nv.h"

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
} capture_entry_t;

static capture_entry_t s_captured[CAPTURE_CAP];
static int             s_capture_count;

static bb_err_t capture_publish(void *ctx, const char *topic,
                                 const char *payload, int len)
{
    (void)ctx;
    (void)len;
    if (s_capture_count >= CAPTURE_CAP) return BB_ERR_NO_SPACE;
    capture_entry_t *e = &s_captured[s_capture_count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    return BB_OK;
}

static void capture_reset(void)
{
    memset(s_captured, 0, sizeof(s_captured));
    s_capture_count = 0;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static void setup(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_mem_register();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_mem_always_publishes(void)
{
    setup();
    bb_pub_tick_once();
    // mem source always returns true — must publish even on host stubs.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_mem_topic_is_correct(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/mem", s_captured[0].topic);
}

void test_bb_pub_mem_has_heap_internal_free(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"heap_internal_free\""));
}

void test_bb_pub_mem_has_heap_internal_min_free(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"heap_internal_min_free\""));
}

void test_bb_pub_mem_has_heap_internal_largest_block(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"heap_internal_largest_block\""));
}

void test_bb_pub_mem_has_uptime_ms(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"uptime_ms\""));
}

void test_bb_pub_mem_omits_psram_free_when_no_psram(void)
{
    setup();
    // Host stub returns psram_total==0 (no PSRAM hardware).
    // psram_free must be absent from the payload.
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"psram_free\""));
}

void test_bb_pub_mem_payload_has_ts_field(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts\""));
}
