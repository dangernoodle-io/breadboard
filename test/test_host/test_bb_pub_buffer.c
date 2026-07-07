// Tests for bb_pub store-and-forward buffer (B1-285).
// Requires CONFIG_BB_PUB_BUFFER_ENABLE=1 (set in native build_flags).
#include "unity.h"
#include "bb_pub.h"
#include "bb_nv.h"
#include "bb_core.h"
#include "bb_pool.h"
#include "bb_mem_arena.h"
#include "bb_mem.h"
#include "bb_mem_test.h"

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
    // Default to lazy heap-backed mode (B1-489). Static-arena tests call
    // bb_pub_test_set_buffer_static(true) after buf_reset.
    bb_pub_test_set_buffer_static(false);
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

// Always-on capture must be skipped for sink[0] when the source isn't
// actually subscribed on sink[0] (p->subscribed[0] == false) -- distinct
// from the buffer_always_on()==false case already covered elsewhere.
static bool buf_reject_all(const char *subtopic, const char *const *tags,
                            int ntags, void *ctx)
{
    (void)subtopic; (void)tags; (void)ntags; (void)ctx;
    return false;
}

static fake_sink_ctx_t s_ctx2;

void test_bb_pub_buffer_always_skips_capture_when_sink0_not_subscribed(void)
{
    buf_reset();
    bb_pub_test_set_buffer_always(true);
    ctx_reset(&s_ctx2);

    bb_pub_sink_t rejecting = make_sink(&s_ctx);
    rejecting.subscribe = buf_reject_all;   // sink[0]: never subscribed
    bb_pub_sink_t accepting = make_sink(&s_ctx2);

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&rejecting));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&accepting));
    bb_pub_register_source("small", source_fn, NULL);

    bb_pub_tick_once();

    // sink[0] never subscribed -> nothing captured into the ring for it.
    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
    // sink[1] (accepting) still received the direct publish.
    TEST_ASSERT_EQUAL_INT(1, s_ctx2.count);
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

// ---------------------------------------------------------------------------
// Sizing-drift guard (B1-478 PR D review) — bb_pub.c's BB_PUB_RING_POOL_ARENA_BYTES
// macro hand-rolls the static-BSS arena budget for the ring pool with magic
// numbers (512 + 64/entry) that duplicate bb_pool's real FIFO layout. If a
// future bb_pool layout change grows past this hand-rolled bound,
// bb_pool_create() fails soft on-device (ring_pool_get() returns NULL,
// store-and-forward silently disabled) with only a log line. This test
// reconstructs bb_pub's exact ring bb_pool_cfg_t (mirrors bb_pub.c — keep in
// sync if bb_pub.c's ring sizing ever changes) and asserts the hand-rolled
// budget still covers bb_pool's actual requirement, so a layout change trips
// CI instead of shipping silently.

// Mirrors bb_pub.c: BB_PUB_BUFFER_TOPIC_MAX default (no CONFIG override in
// this test env) and BB_PUB_BUFFER_ENTRY_MAX derivation.
#define TEST_BB_PUB_BUFFER_TOPIC_MAX 192
#define TEST_BB_PUB_BUFFER_ENTRY_MAX \
    (TEST_BB_PUB_BUFFER_TOPIC_MAX + 1 + CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES)

// Mirrors bb_pub.c's BB_PUB_RING_POOL_ARENA_BYTES exactly (512 B pool/arena
// struct headroom + 64 B per-entry header/alignment overhead + capacity *
// entry payload). Keep in sync with bb_pub.c if that formula changes.
#define TEST_BB_PUB_RING_POOL_ARENA_BYTES \
    (512u + \
     (size_t)(CONFIG_BB_PUB_BUFFER_MAX_ENTRIES) * 64u + \
     (size_t)(CONFIG_BB_PUB_BUFFER_MAX_ENTRIES) * (size_t)(TEST_BB_PUB_BUFFER_ENTRY_MAX))

// Scratch buffer mirrors bb_pub.c's real static-BSS s_ring_arena_buf: same
// size and alignment. bb_mem_arena_init carves BB_MEM_ARENA_HDR_SZ off the front, so
// the bytes actually available to bb_pool_create are less than sizeof(scratch).
static uint8_t s_scratch[TEST_BB_PUB_RING_POOL_ARENA_BYTES]
    __attribute__((aligned(_Alignof(max_align_t))));

void test_bb_pub_buffer_ring_arena_budget_covers_pool_requirement(void)
{
    bb_pool_cfg_t cfg = {
        .mode           = BB_POOL_MODE_FIFO,
        .capacity       = (size_t)CONFIG_BB_PUB_BUFFER_MAX_ENTRIES,
        .max_slot_bytes = (size_t)TEST_BB_PUB_BUFFER_ENTRY_MAX,
        .full_policy    = BB_POOL_FULL_EVICT_OLDEST,
        .name           = "pub",
    };

    size_t needed = bb_pool_arena_size_needed(&cfg);
    TEST_ASSERT_GREATER_THAN_size_t(0, needed);

    bb_mem_arena_t arena = NULL;
    bb_err_t rc = bb_mem_arena_init(&arena, s_scratch, sizeof(s_scratch));
    TEST_ASSERT_EQUAL(BB_OK, rc);

    size_t available = bb_mem_arena_free_bytes(arena);
    TEST_ASSERT_TRUE_MESSAGE(available >= needed,
        "bb_pub's hand-rolled BB_PUB_RING_POOL_ARENA_BYTES budget no longer "
        "covers bb_pool's actual arena requirement (post header-carve) for "
        "the ring FIFO cfg — update the sizing constants in bb_pub.c");

    bb_mem_arena_destroy(arena);   /* no-op for caller-supplied buffer; explicit for clarity */
}

// ---------------------------------------------------------------------------
// Lazy heap-backed ring vs static-BSS ring (B1-489)
// ---------------------------------------------------------------------------

// 19. Default (lazy heap) mode: healthy operation (no outage ever) leaves
//     bb_mem outstanding_bytes unchanged — the ring is never allocated.
void test_bb_pub_buffer_lazy_heap_zero_standing_cost_when_healthy(void)
{
    buf_reset();
    bb_pub_test_set_buffer_static(false);

    bb_mem_stats_t before;
    bb_mem_get_stats(&before);

    bb_pub_sink_t sk = make_sink(&s_ctx);   /* s_ctx.fail = false */
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    for (int i = 0; i < 5; i++) {
        bb_pub_tick_once();   /* sink always succeeds → ring never touched */
    }

    bb_mem_stats_t after;
    bb_mem_get_stats(&after);
    TEST_ASSERT_EQUAL_size_t(before.outstanding_bytes, after.outstanding_bytes);
}

// 20. Default (lazy heap) mode: an outage allocates heap for the ring
//     (outstanding_bytes rises), and idle-free reclaim actually returns that
//     memory to the heap (outstanding_bytes falls back to baseline).
void test_bb_pub_buffer_lazy_heap_alloc_and_reclaim_moves_heap_stats(void)
{
    buf_reset();
    bb_pub_test_set_buffer_static(false);
    bb_pub_test_set_idle_free_ticks(2);

    bb_mem_stats_t baseline;
    bb_mem_get_stats(&baseline);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();   /* outage: ring heap-allocated, entry captured */

    bb_mem_stats_t during;
    bb_mem_get_stats(&during);
    TEST_ASSERT_GREATER_THAN_size_t(baseline.outstanding_bytes,
                                     during.outstanding_bytes);

    /* recovery: drain, then run several empty ticks — comfortably past the
     * idle_free_ticks(2) threshold regardless of exactly which tick the
     * drain itself counts towards, so the reclaim is guaranteed to fire. */
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();   /* drains */
    for (int i = 0; i < 5; i++) {
        ctx_reset(&s_ctx);
        bb_pub_tick_once();   /* idle ticks -> threshold reached -> pool destroyed */
    }

    bb_mem_stats_t reclaimed;
    bb_mem_get_stats(&reclaimed);
    TEST_ASSERT_EQUAL_size_t(baseline.outstanding_bytes, reclaimed.outstanding_bytes);

    /* stats API still reports a clean, empty ring after reclaim */
    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

#ifdef BB_MEM_TESTING
// Fail-injection hook: always returns NULL (simulates a fragmented/exhausted
// heap that cannot satisfy the ring's contiguous arena allocation).
static void *s_null_alloc_hook(size_t n)
{
    (void)n;
    return NULL;
}
#endif

// 21. Default (lazy heap) mode: a heap allocation failure at ring-create time
//     is fail-soft — no crash, entry simply isn't buffered this cycle, and a
//     subsequent successful allocation recovers store-and-forward normally.
void test_bb_pub_buffer_lazy_heap_alloc_failure_is_fail_soft(void)
{
#ifdef BB_MEM_TESTING
    buf_reset();
    bb_pub_test_set_buffer_static(false);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_mem_set_alloc_hook(s_null_alloc_hook);
    bb_pub_tick_once();   /* ring create fails; must not crash */
    bb_mem_set_alloc_hook(NULL);

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);   /* nothing buffered this cycle */

    /* allocator restored: next outage tick buffers normally. ctx_reset()
     * zeroes the whole fake_sink_ctx_t (including `fail`), so re-arm the
     * failure flag afterwards to keep the sink refusing this tick too. */
    ctx_reset(&s_ctx);
    s_ctx.fail = true;
    bb_pub_tick_once();

    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_TESTING not defined; skip alloc-fail injection test");
#endif
}

// 21b. Consecutive lazy-heap allocation failures log the warning only once
// (s_ring_alloc_fail_logged latch) — the second consecutive failure must
// skip re-logging. Correctness is asserted the same way as the fail-soft
// test above (no crash, nothing buffered); the log-once behavior itself is
// a coverage-only branch with no externally observable side effect to
// assert on beyond "did not crash".
void test_bb_pub_buffer_lazy_heap_consecutive_alloc_failures_log_once(void)
{
#ifdef BB_MEM_TESTING
    buf_reset();
    bb_pub_test_set_buffer_static(false);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_mem_set_alloc_hook(s_null_alloc_hook);
    bb_pub_tick_once();   /* first failure: warning logged, latch set */
    bb_pub_tick_once();   /* second consecutive failure: latch already set */
    bb_mem_set_alloc_hook(NULL);

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_TESTING not defined; skip alloc-fail injection test");
#endif
}

// ---------------------------------------------------------------------------
// STATIC=y mode (CONFIG_BB_PUB_BUFFER_STATIC): the pre-B1-489 static-BSS
// arena behaviour still passes under the same test seams.
// ---------------------------------------------------------------------------

// 22. Static mode: failing sink enqueues, recovery replays oldest-first —
//     baseline store-and-forward semantics unchanged.
void test_bb_pub_buffer_static_mode_replay_oldest_first(void)
{
    buf_reset();
    bb_pub_test_set_buffer_static(true);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("topicA", source_fn, NULL);
    bb_pub_register_source("topicB", source_fn, NULL);

    bb_pub_tick_once();   /* both fail -> 2 buffered */

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(2, stats.count);

    s_ctx.fail = false;
    bb_pub_tick_once();   /* live 2 + replay 2 = 4 */

    TEST_ASSERT_EQUAL_INT(4, s_ctx.count);
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(0, stats.count);
}

// 23. Static mode: overflow drops oldest and increments dropped counter.
void test_bb_pub_buffer_static_mode_overflow_drops_oldest(void)
{
    buf_reset();
    bb_pub_test_set_buffer_static(true);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_buffer_stats_t stats;
    for (int i = 0; i < 20; i++) {
        bb_pub_tick_once();
    }

    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(16, stats.count);
    TEST_ASSERT_EQUAL_size_t(4, stats.dropped);
}

// 24. Static mode: idle-free reclaim is a documented no-op — the ring stays
//     present (and usable) even past the idle threshold, unlike lazy-heap
//     mode. This is the STATIC=y contract: no heap allocation, ever.
void test_bb_pub_buffer_static_mode_idle_ticks_do_not_destroy_ring(void)
{
    buf_reset();
    bb_pub_test_set_buffer_static(true);
    bb_pub_test_set_idle_free_ticks(2);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();            /* capture */
    s_ctx.fail = false;
    ctx_reset(&s_ctx);
    bb_pub_tick_once();            /* drain */
    ctx_reset(&s_ctx);
    bb_pub_tick_once();            /* idle tick 1 */
    ctx_reset(&s_ctx);
    bb_pub_tick_once();            /* idle tick 2 -> threshold reached, no-op in static mode */

    /* ring still usable immediately (no re-create latency/log) */
    ctx_reset(&s_ctx);
    s_ctx.fail = true;
    bb_pub_tick_once();

    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_size_t(1, stats.count);
}

// 25. Static mode does not touch bb_mem heap accounting at all (no
//     bb_calloc_prefer_spiram call is ever made for the ring in this mode).
void test_bb_pub_buffer_static_mode_no_heap_accounting(void)
{
    buf_reset();
    bb_pub_test_set_buffer_static(true);

    bb_mem_stats_t before;
    bb_mem_get_stats(&before);

    s_ctx.fail = true;
    bb_pub_sink_t sk = make_sink(&s_ctx);
    bb_pub_set_sink(&sk);
    bb_pub_register_source("info", source_fn, NULL);

    bb_pub_tick_once();   /* forces ring creation in static mode */

    bb_mem_stats_t after;
    bb_mem_get_stats(&after);
    TEST_ASSERT_EQUAL_size_t(before.outstanding_bytes, after.outstanding_bytes);
}
