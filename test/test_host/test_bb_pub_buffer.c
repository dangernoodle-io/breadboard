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
                              const char *payload, int len, bool retain)
{
    (void)retain;
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
    // Default to on-failure mode for tests that don't explicitly set always-on.
    // Always-on tests call bb_pub_test_set_buffer_always(true) after buf_reset.
    bb_pub_test_set_buffer_always(false);
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

// ---------------------------------------------------------------------------
// Always-on mode tests (BB_PUB_BUFFER_ALWAYS via runtime seam)
// ---------------------------------------------------------------------------

// 11. Always-on: sources enqueue then drain same tick → delivered in order, no
//     captured_ms on fresh points (age << 1.5 × interval).
void test_bb_pub_buffer_always_healthy_no_captured_ms(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);

    // NTP-synced epoch so captured_ms WOULD be injected if logic is wrong.
    const int64_t fake_epoch = 1700000000000LL;
    bb_pub_test_set_synced_epoch_ms(fake_epoch);

    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("topicA", source_fn, NULL);
    bb_pub_register_source("topicB", source_fn, NULL);

    bb_pub_tick_once();   /* enqueue 2 + drain same tick */

    // Both entries should be delivered to the sink.
    TEST_ASSERT_EQUAL_INT(2, s_ctx.count);

    // Ring should be empty after drain.
    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);

    // No captured_ms: entries were fresh (same-tick drain).
    for (int i = 0; i < s_ctx.count; i++) {
        TEST_ASSERT_NULL_MESSAGE(
            strstr(s_ctx.entries[i].payload, "captured_ms"),
            "fresh always-on point must not have captured_ms");
    }
}

// 12. Always-on: source ordering preserved across tick (enqueue order = drain
//     order = delivery order).
void test_bb_pub_buffer_always_delivery_order(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);

    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("first",  source_fn, NULL);
    bb_pub_register_source("second", source_fn, NULL);
    bb_pub_register_source("third",  source_fn, NULL);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(3, s_ctx.count);
    TEST_ASSERT_NOT_NULL(strstr(s_ctx.entries[0].topic, "first"));
    TEST_ASSERT_NOT_NULL(strstr(s_ctx.entries[1].topic, "second"));
    TEST_ASSERT_NOT_NULL(strstr(s_ctx.entries[2].topic, "third"));
}

// 13. Always-on outage → recovery: delayed entries carry captured_ms; fresh
//     entries drained the same tick they are enqueued do not.
void test_bb_pub_buffer_always_outage_recovery_captured_ms(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);

    const int64_t epoch_t0 = 1700000000000LL;
    // interval_ms default = CONFIG_BB_PUB_INTERVAL_MS = 10000 ms
    // 1.5 × interval = 15 000 ms.  Advance epoch by 20 000 ms to exceed threshold.
    const int64_t epoch_t1 = epoch_t0 + 20000LL;

    bb_pub_test_set_synced_epoch_ms(epoch_t0);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();   /* enqueue: entry_ts = epoch_t0; drain fails */

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);   /* still in ring */

    // Advance epoch to simulate time passing; drain on next tick succeeds.
    bb_pub_test_set_synced_epoch_ms(epoch_t1);
    s_ctx.fail = false;
    ctx_reset(&s_ctx);

    bb_pub_tick_once();   /* enqueue fresh (ts=epoch_t1) + drain old + new */

    // 1 delayed entry (from tick 1) + 1 fresh entry (from tick 2) = 2 delivered.
    TEST_ASSERT_EQUAL_INT(2, s_ctx.count);

    // The delayed entry (epoch_t0 captured, now epoch_t1, age = 20000 > 15000)
    // must carry captured_ms.
    char expected[64];
    snprintf(expected, sizeof(expected), "\"captured_ms\":%lld", (long long)epoch_t0);
    bool delayed_has_captured_ms = false;
    bool fresh_has_captured_ms   = false;
    for (int i = 0; i < s_ctx.count; i++) {
        if (strstr(s_ctx.entries[i].payload, expected)) {
            delayed_has_captured_ms = true;
        }
        // Fresh entry was captured at epoch_t1; any captured_ms on it is wrong.
        char fresh_expected[64];
        snprintf(fresh_expected, sizeof(fresh_expected),
                 "\"captured_ms\":%lld", (long long)epoch_t1);
        if (strstr(s_ctx.entries[i].payload, fresh_expected)) {
            fresh_has_captured_ms = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(delayed_has_captured_ms,
        "delayed always-on entry must have captured_ms");
    TEST_ASSERT_FALSE_MESSAGE(fresh_has_captured_ms,
        "fresh always-on entry (same-tick drain) must not have captured_ms");

    // Ring empty after successful drain.
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

// 14. Always-on ring-full eviction: oldest evicted, dropped counter increments.
void test_bb_pub_buffer_always_overflow(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);

    // Make sink always fail so ring fills up.
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
    TEST_ASSERT_EQUAL_size_t(4,  stats.dropped);
}

// 15. Always-on: last_publish_ok reflects whether the ring drained fully.
void test_bb_pub_buffer_always_last_publish_ok(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);

    // Tick 1: sink fails → ring not drained → last_publish_ok = false.
    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_FALSE_MESSAGE(st.last_publish_ok,
        "always-on with failing sink should report last_publish_ok=false");

    // Tick 2: sink succeeds → ring drains → last_publish_ok = true.
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();

    bb_pub_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.last_publish_ok,
        "always-on with succeeding sink should report last_publish_ok=true");
}

// 16. Regression: ALWAYS=n (on-failure, default) behavior unchanged —
//     existing store-and-forward semantics still hold.
void test_bb_pub_buffer_always_off_regression(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(false);   /* force on-failure mode */

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

// ---------------------------------------------------------------------------
// Oversized-entry fallback tests (B1-oversize-fallback)
// ---------------------------------------------------------------------------

// Source that emits a payload large enough to exceed BB_PUB_BUFFER_ENTRY_MAX.
// We add a string field with 300 'x' characters; together with the topic and
// the mandatory "uptime_ms" field the serialized entry will exceed the ring slot.
static bool oversized_source_fn(bb_json_t obj, void *ctx)
{
    (void)ctx;
    // 300-char value: ensures serialized JSON >> CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES (256).
    char big[301];
    memset(big, 'x', 300);
    big[300] = '\0';
    bb_json_obj_set_string(obj, "data", big);
    return true;
}

// 17. Always-on: oversized entry is published directly instead of being
//     dropped.  buffer_capture rejects it (entry_len > BB_PUB_BUFFER_ENTRY_MAX)
//     so the always-on path must fall through to a direct sk->publish call.
void test_bb_pub_buffer_always_oversized_published_directly(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);

    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("big", oversized_source_fn, NULL);

    bb_pub_tick_once();

    // The oversized entry must have been delivered directly — not dropped.
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_ctx.count,
        "always-on oversized entry must be published directly (not dropped)");

    // The payload must contain the oversized field.
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(s_ctx.entries[0].payload, "\"data\""),
        "oversized direct-publish payload must contain the data field");

    // Ring must be empty: the oversized entry was never enqueued.
    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

// 18. Always-on: a normal-sized payload still goes through the ring
//     (enqueue → drain), not via direct publish.  This confirms the
//     fallback path only activates for oversized entries.
void test_bb_pub_buffer_always_normal_goes_through_ring(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);

    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("small", source_fn, NULL);   /* tiny payload */

    // Tick with sink failing: entry should be enqueued in ring, not published.
    s_ctx.fail = true;
    bb_pub_tick_once();

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);   /* enqueued in ring */
    TEST_ASSERT_EQUAL_INT(0, s_ctx.count);      /* NOT published directly */

    // Now succeed: ring drains normally.
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();   /* 1 live + 1 drained = 2 */

    TEST_ASSERT_EQUAL_INT(2, s_ctx.count);
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);   /* ring empty */
}

// ---------------------------------------------------------------------------
// Idle-free tests (B1-419) — free store-forward ring when idle
// ---------------------------------------------------------------------------

// (a) Ring is created on first failure (lazy allocation still works, pre-condition
//     for the idle-free tests). Ring must be non-NULL after one failing tick.
void test_bb_pub_buffer_idle_ring_created_on_first_failure(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(false);
    bb_pub_test_set_idle_free_ticks(5);   /* threshold above 1 so ring survives */

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();   /* sink fails → entry captured */

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);   /* ring allocated and has an entry */
}

// (b) Ring is destroyed after N empty ticks post-drain.
void test_bb_pub_buffer_idle_ring_freed_after_n_idle_ticks(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(false);
    bb_pub_test_set_idle_free_ticks(3);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    /* tick 1: capture an entry (sink fails) */
    bb_pub_tick_once();

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);   /* entry buffered */

    /* recovery: sink succeeds → ring drains */
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();   /* tick 2: live + replay; ring now empty */

    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);   /* ring empty but still allocated */

    /* idle ticks 1 and 2: ring stays empty (below threshold=3) */
    ctx_reset(&s_ctx);
    bb_pub_tick_once();   /* idle tick 1 */
    ctx_reset(&s_ctx);
    bb_pub_tick_once();   /* idle tick 2 */

    /* ring should still exist (only 2 idle ticks, threshold is 3) */
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);

    /* idle tick 3: counter reaches threshold → ring freed */
    ctx_reset(&s_ctx);
    bb_pub_tick_once();

    /* after free, s_buffer is NULL → stats report count=0 */
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);

    /* confirm the ring is really gone: a new failure must re-allocate it */
    ctx_reset(&s_ctx);
    s_ctx.fail = true;
    bb_pub_tick_once();   /* re-allocates ring and captures */

    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);   /* ring re-created with entry */
}

// (c) Ring is re-created on subsequent failure after being freed.
void test_bb_pub_buffer_idle_ring_recreated_after_free(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(false);
    bb_pub_test_set_idle_free_ticks(2);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    /* capture → drain → 2 idle ticks → ring freed */
    bb_pub_tick_once();            /* capture: entry buffered */
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();            /* drain: ring empty */
    ctx_reset(&s_ctx);
    bb_pub_tick_once();            /* idle tick 1 */
    ctx_reset(&s_ctx);
    bb_pub_tick_once();            /* idle tick 2 → ring freed */

    /* confirm ring is freed: first stats call sees count=0 */
    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);

    /* second outage → ring must be re-created */
    ctx_reset(&s_ctx);
    s_ctx.fail = true;
    bb_pub_tick_once();

    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);   /* re-created and has entry */

    /* recovery from second outage → ring drains again */
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(2, s_ctx.count);   /* live + replayed */
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

// (d) BUFFER_ALWAYS=y — ring must NOT be freed even after many idle ticks.
void test_bb_pub_buffer_idle_always_on_not_freed(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);    /* always-on mode */
    bb_pub_test_set_idle_free_ticks(2);     /* low threshold would trigger if logic wrong */

    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    /* run more ticks than the idle threshold; sink always succeeds → ring
     * drains every tick (always-on: enqueue→drain). */
    for (int i = 0; i < 5; i++) {
        ctx_reset(&s_ctx);
        bb_pub_tick_once();
    }

    /* ring must remain allocated: stats return count=0 (drained), not absent */
    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    /* in always-on mode the ring drains each tick → count 0, but NOT destroyed */
    TEST_ASSERT_EQUAL_size_t(0, stats.count);

    /* confirm: a subsequent tick still works (ring present → enqueue succeeds) */
    ctx_reset(&s_ctx);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_ctx.count,
        "always-on: ring must still be present after idle ticks");
}

// (e) IDLE_FREE_TICKS=0 disables the idle reclaim.
void test_bb_pub_buffer_idle_zero_ticks_disables_reclaim(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(false);
    bb_pub_test_set_idle_free_ticks(0);   /* 0 = disabled */

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    /* capture → drain → many idle ticks */
    bb_pub_tick_once();
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();   /* drains */

    /* run 10 idle ticks; ring must NOT be freed */
    for (int i = 0; i < 10; i++) {
        ctx_reset(&s_ctx);
        bb_pub_tick_once();
    }

    /* ring still allocated: a new failure can capture immediately */
    ctx_reset(&s_ctx);
    s_ctx.fail = true;
    bb_pub_tick_once();

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);
    TEST_ASSERT_EQUAL_size_t(0, stats.dropped);
}
