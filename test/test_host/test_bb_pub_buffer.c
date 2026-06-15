// Tests for bb_pub store-and-forward buffer (B1-285).
// Requires CONFIG_BB_PUB_BUFFER_ENABLE=1 (set in native build_flags).
#include "unity.h"
#include "bb_pub.h"
#include "bb_nv.h"
#include "bb_core.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Helpers — fake sinks
// ---------------------------------------------------------------------------

#define CAPS_MAX 64

typedef struct {
    char  topic[192];
    char  payload[1024];
    int   len;
} cap_entry_t;

typedef struct {
    cap_entry_t entries[CAPS_MAX];
    int         count;
    bool        fail;   /* when true, publish returns BB_ERR_TIMEOUT */
} fake_sink_ctx_t;

static bb_err_t fake_publish(void *ctx, const char *topic,
                              const char *payload, int len)
{
    fake_sink_ctx_t *c = (fake_sink_ctx_t *)ctx;
    if (c->fail) return BB_ERR_TIMEOUT;
    if (c->count >= CAPS_MAX) return BB_ERR_NO_SPACE;
    cap_entry_t *e = &c->entries[c->count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    e->len = len;
    return BB_OK;
}

static void ctx_reset(fake_sink_ctx_t *c)
{
    memset(c, 0, sizeof(*c));
}

// Minimal source that always publishes.
static bool source_fn(bb_json_t obj, void *ctx)
{
    (void)ctx;
    (void)obj;
    return true;
}

static bb_pub_sink_t make_sink(fake_sink_ctx_t *c)
{
    bb_pub_sink_t s = { .publish = fake_publish, .ctx = c };
    return s;
}

// ---------------------------------------------------------------------------
// Per-test reset
// ---------------------------------------------------------------------------

static fake_sink_ctx_t s_ctx;

static void buf_reset(void)
{
    bb_pub_test_reset();
    bb_pub_test_set_synced_epoch_ms(-1);   /* not synced */
    ctx_reset(&s_ctx);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. Failing sink enqueues a buffered entry.
void test_bb_pub_buffer_failing_sink_enqueues(void)
{
    buf_reset();
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);
    TEST_ASSERT_EQUAL_size_t(0, stats.dropped);
    TEST_ASSERT_EQUAL_INT(0, s_ctx.count);
}

// 2. Recovery replays oldest-first.
void test_bb_pub_buffer_replay_oldest_first(void)
{
    buf_reset();
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("topicA", source_fn, NULL);
    bb_pub_register_source("topicB", source_fn, NULL);

    bb_pub_tick_once();   /* both fail → 2 buffered */

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(2, stats.count);

    s_ctx.fail = false;
    bb_pub_tick_once();   /* live 2 + replay 2 = 4 */

    TEST_ASSERT_EQUAL_INT(4, s_ctx.count);

    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

// 3. Overflow drops oldest and increments dropped counter.
void test_bb_pub_buffer_overflow_drops_oldest(void)
{
    buf_reset();
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    // Ring capacity = 16; tick 20 times → 4 dropped.
    for (int i = 0; i < 20; i++) {
        bb_pub_tick_once();
    }

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(16, stats.count);
    TEST_ASSERT_EQUAL_size_t(4, stats.dropped);
}

// 4. Paused → no capture.
void test_bb_pub_buffer_paused_no_capture(void)
{
    buf_reset();
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_pause();
    bb_pub_tick_once();
    bb_pub_resume();

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

// 5. Disabled → no capture.
void test_bb_pub_buffer_disabled_no_capture(void)
{
    buf_reset();
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_set_enabled(false);
    bb_pub_tick_once();
    bb_pub_set_enabled(true);

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

// 6. Normal payload accepted (no truncation).
void test_bb_pub_buffer_normal_payload_accepted(void)
{
    buf_reset();
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);
    TEST_ASSERT_EQUAL_size_t(0, stats.truncated);
}

// 7. Capture epoch is injected into replayed payload when NTP is synced.
void test_bb_pub_buffer_capture_epoch_injected_on_replay(void)
{
    buf_reset();
    const int64_t fake_epoch = 1700000000000LL;
    bb_pub_test_set_synced_epoch_ms(fake_epoch);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();   /* captured with epoch */

    s_ctx.fail = false;
    ctx_reset(&s_ctx);

    bb_pub_tick_once();   /* live + replay */

    bool found = false;
    char expected[64];
    snprintf(expected, sizeof(expected), "\"captured_ms\":%lld", (long long)fake_epoch);
    for (int i = 0; i < s_ctx.count; i++) {
        if (strstr(s_ctx.entries[i].payload, expected)) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "expected 'captured_ms' in replayed payload");
}

// 8. When epoch is 0 (not synced), no captured_ms is injected on replay.
void test_bb_pub_buffer_no_epoch_no_captured_ms_field(void)
{
    buf_reset();
    bb_pub_test_set_synced_epoch_ms(0);   /* not synced → ts=0 */

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();

    s_ctx.fail = false;
    ctx_reset(&s_ctx);

    bb_pub_tick_once();

    bool has = false;
    for (int i = 0; i < s_ctx.count; i++) {
        if (strstr(s_ctx.entries[i].payload, "captured_ms")) {
            has = true;
            break;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(has, "captured_ms should not appear when epoch=0");
}

// 9. Stats reflect count and dropped correctly.
void test_bb_pub_buffer_stats_reflect_reality(void)
{
    buf_reset();
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    for (int i = 0; i < 5; i++) {
        bb_pub_tick_once();
    }

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(5, stats.count);
    TEST_ASSERT_EQUAL_size_t(0, stats.dropped);

    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();   /* drains all 5 */

    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

// 10. Multi-tick failure accumulates; successful tick drains all.
void test_bb_pub_buffer_replay_stops_on_failure(void)
{
    buf_reset();
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();   /* 1 buffered */

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);

    // Second failure tick → 2 buffered.
    ctx_reset(&s_ctx);
    s_ctx.fail = true;
    bb_pub_tick_once();

    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(2, stats.count);

    // Successful tick: 1 live + 2 replay = 3 total.
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(3, s_ctx.count);
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}
