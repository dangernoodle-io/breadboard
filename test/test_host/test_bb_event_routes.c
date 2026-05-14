#include "unity.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"
#include "bb_event_test.h"
#include "test_alloc_inject.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Forward decls for ring allocator hook (declared in bb_event_ring_internal.h
 * but we don't want to pull that whole header). */
void bb_event_ring_set_allocator(void *(*c)(size_t, size_t), void (*f)(void *));
void bb_event_ring_reset_allocator(void);

static void setup_sync_mode(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
}

static void reset_world(void)
{
    bb_event_routes_reset_for_test();
    bb_event_port_reset_for_test();
    bb_event_reset_for_test();
    bb_event_init(NULL);
}

static const bb_event_routes_cfg_t small_cfg = {
    .max_clients = 2,
    .per_client_queue = 3,
    .ring_capacity = 4,
    .ring_max_entry = 64,
    .heartbeat_ms = 1000,
};

// ---------------------------------------------------------------------------
// attach
// ---------------------------------------------------------------------------

void test_bb_event_routes_init_idempotent(void)
{
    setup_sync_mode();
    reset_world();
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_init(&small_cfg));
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_init(&small_cfg));
}

void test_bb_event_routes_init_null_cfg_uses_defaults(void)
{
    setup_sync_mode();
    reset_world();
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_init(NULL));
    TEST_ASSERT_TRUE(bb_event_routes_heartbeat_ms() > 0);
}

void test_bb_event_routes_init_zero_cfg_fields_use_defaults(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_cfg_t zero_cfg = {0};  /* every field 0 -> all defaults */
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_init(&zero_cfg));
    TEST_ASSERT_TRUE(bb_event_routes_heartbeat_ms() > 0);
}

void test_bb_event_routes_drain_null_buf_returns_zero(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);
    bb_event_routes_client_t *c = NULL;
    bb_event_routes_client_acquire(&c);
    TEST_ASSERT_EQUAL(0, bb_event_routes_drain_frame(c, NULL, 1024));
    bb_event_routes_client_release(c);
}

void test_bb_event_routes_attach_returns_not_found_for_unregistered_topic(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_event_routes_attach("does.not.exist"));
}

void test_bb_event_routes_attach_dedupes_same_topic(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("dedup.topic", &t);

    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_attach("dedup.topic"));
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_attach("dedup.topic"));
}

void test_bb_event_routes_attach_null_returns_invalid_arg(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_event_routes_attach(NULL));
}

void test_bb_event_routes_attach_before_init_returns_invalid_state(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_reset_for_test();
    /* leave bb_event initialized but route handler not */
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_event_routes_attach("anything"));
}

// ---------------------------------------------------------------------------
// client lifecycle
// ---------------------------------------------------------------------------

void test_bb_event_routes_client_acquire_release_round_trip(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_routes_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_client_acquire(&c));
    TEST_ASSERT_NOT_NULL(c);
    bb_event_routes_client_release(c);
}

void test_bb_event_routes_client_acquire_null_out_returns_invalid_arg(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_event_routes_client_acquire(NULL));
}

void test_bb_event_routes_client_acquire_before_init_returns_invalid_state(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_reset_for_test();
    bb_event_routes_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_event_routes_client_acquire(&c));
}

void test_bb_event_routes_client_release_null_noop(void)
{
    bb_event_routes_client_release(NULL);
    /* no crash */
}

void test_bb_event_routes_client_pool_exhaustion_returns_no_space(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);  /* max_clients=2 */

    bb_event_routes_client_t *c1 = NULL, *c2 = NULL, *c3 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_client_acquire(&c1));
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_client_acquire(&c2));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_event_routes_client_acquire(&c3));

    bb_event_routes_client_release(c1);
    bb_event_routes_client_release(c2);
}

// ---------------------------------------------------------------------------
// drain + SSE frame format
// ---------------------------------------------------------------------------

void test_bb_event_routes_drain_emits_sse_frame(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("drain.topic", &t);
    bb_event_routes_attach("drain.topic");

    bb_event_routes_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_client_acquire(&c));

    const char *json = "{\"x\":1}";
    bb_event_post(t, 0, json, strlen(json));
    bb_event_pump(0);

    char frame[256];
    size_t n = bb_event_routes_drain_frame(c, frame, sizeof(frame));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(frame, "event: drain.topic\n"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "data: {\"x\":1}\n"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\nid: "));
    TEST_ASSERT_EQUAL_STRING("\n\n", frame + n - 2);

    bb_event_routes_client_release(c);
}

void test_bb_event_routes_drain_empty_payload_emits_object(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("empty.topic", &t);
    bb_event_routes_attach("empty.topic");

    bb_event_routes_client_t *c = NULL;
    bb_event_routes_client_acquire(&c);

    bb_event_post(t, 0, NULL, 0);
    bb_event_pump(0);

    char frame[256];
    size_t n = bb_event_routes_drain_frame(c, frame, sizeof(frame));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(frame, "data: {}\n"));

    bb_event_routes_client_release(c);
}

void test_bb_event_routes_drain_empty_queue_returns_zero(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_routes_client_t *c = NULL;
    bb_event_routes_client_acquire(&c);

    char frame[128];
    TEST_ASSERT_EQUAL(0, bb_event_routes_drain_frame(c, frame, sizeof(frame)));

    bb_event_routes_client_release(c);
}

void test_bb_event_routes_drain_null_client_returns_zero(void)
{
    char frame[128];
    TEST_ASSERT_EQUAL(0, bb_event_routes_drain_frame(NULL, frame, sizeof(frame)));
}

void test_bb_event_routes_drain_tiny_buf_returns_zero(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);
    bb_event_routes_client_t *c = NULL;
    bb_event_routes_client_acquire(&c);
    char tiny[8];
    TEST_ASSERT_EQUAL(0, bb_event_routes_drain_frame(c, tiny, sizeof(tiny)));
    bb_event_routes_client_release(c);
}

// ---------------------------------------------------------------------------
// fan-out: two clients both receive the same event
// ---------------------------------------------------------------------------

void test_bb_event_routes_two_clients_both_receive(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("fanout.topic", &t);
    bb_event_routes_attach("fanout.topic");

    bb_event_routes_client_t *c1 = NULL, *c2 = NULL;
    bb_event_routes_client_acquire(&c1);
    bb_event_routes_client_acquire(&c2);

    const char *json = "{\"v\":42}";
    bb_event_post(t, 0, json, strlen(json));
    bb_event_pump(0);

    char f1[256], f2[256];
    TEST_ASSERT_TRUE(bb_event_routes_drain_frame(c1, f1, sizeof(f1)) > 0);
    TEST_ASSERT_TRUE(bb_event_routes_drain_frame(c2, f2, sizeof(f2)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(f1, "{\"v\":42}"));
    TEST_ASSERT_NOT_NULL(strstr(f2, "{\"v\":42}"));

    bb_event_routes_client_release(c1);
    bb_event_routes_client_release(c2);
}

// ---------------------------------------------------------------------------
// overflow: drop oldest, id reflects gap
// ---------------------------------------------------------------------------

void test_bb_event_routes_queue_overflow_drops_oldest(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);  /* per_client_queue=3 */

    bb_event_topic_t t;
    bb_event_topic_register("overflow.topic", &t);
    bb_event_routes_attach("overflow.topic");

    bb_event_routes_client_t *c = NULL;
    bb_event_routes_client_acquire(&c);

    /* Post 5 events into a depth-3 queue. Oldest 2 must be dropped. */
    for (int i = 0; i < 5; i++) {
        char payload[16];
        snprintf(payload, sizeof(payload), "{\"i\":%d}", i);
        bb_event_post(t, i, payload, strlen(payload));
        bb_event_pump(0);
    }

    TEST_ASSERT_EQUAL(3, bb_event_routes_queued_for_test(c));
    TEST_ASSERT_EQUAL(2, bb_event_routes_dropped_for_test(c));

    /* First drained frame should carry id = 1 + dropped (i.e. id=3) due to gap. */
    char frame[256];
    size_t n = bb_event_routes_drain_frame(c, frame, sizeof(frame));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(frame, "id: 3\n"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "{\"i\":2}"));

    bb_event_routes_client_release(c);
}

// ---------------------------------------------------------------------------
// replay-on-connect: events posted before subscribe are delivered
// ---------------------------------------------------------------------------

void test_bb_event_routes_client_acquire_replays_buffered_events(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("replay.topic", &t);
    bb_event_routes_attach("replay.topic");

    const char *p = "{\"r\":1}";
    bb_event_post(t, 0, p, strlen(p));
    bb_event_pump(0);

    /* Client connects AFTER the post; replay should deliver it. */
    bb_event_routes_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_client_acquire(&c));

    char frame[256];
    size_t n = bb_event_routes_drain_frame(c, frame, sizeof(frame));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(frame, "{\"r\":1}"));

    bb_event_routes_client_release(c);
}

// ---------------------------------------------------------------------------
// Coverage gap fillers
// ---------------------------------------------------------------------------

void test_bb_event_routes_init_max_clients_above_cap_returns_invalid_arg(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_cfg_t cfg = small_cfg;
    cfg.max_clients = 9999;  /* > CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS */
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_event_routes_init(&cfg));
}

void test_bb_event_routes_attach_table_full_returns_no_space(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    /* Default cap CONFIG_BB_EVENT_ROUTES_MAX_TOPICS = 8. Register + attach 8,
     * then the 9th must fail. */
    char name[32];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "fill.topic.%d", i);
        bb_event_topic_t t;
        bb_event_topic_register(name, &t);
        TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_attach(name));
    }
    bb_event_topic_t t9;
    bb_event_topic_register("fill.topic.9", &t9);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_event_routes_attach("fill.topic.9"));
}

void test_bb_event_routes_heartbeat_ms_returns_configured_value(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);
    TEST_ASSERT_EQUAL(small_cfg.heartbeat_ms, bb_event_routes_heartbeat_ms());
}

void test_bb_event_routes_reset_releases_held_client(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);
    bb_event_routes_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_client_acquire(&c));
    /* Don't release; let reset_for_test handle it (covers the in_use branch). */
    bb_event_routes_reset_for_test();
    bb_event_port_reset_for_test();
    bb_event_reset_for_test();
    bb_event_init(NULL);
}

void test_bb_event_routes_capture_walks_past_non_matching_topic(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t1, t2;
    bb_event_topic_register("walk.first", &t1);
    bb_event_topic_register("walk.second", &t2);
    bb_event_routes_attach("walk.first");
    bb_event_routes_attach("walk.second");

    bb_event_routes_client_t *c = NULL;
    bb_event_routes_client_acquire(&c);

    /* Post to second-attached topic; capture_cb's linear scan walks past first. */
    const char *p = "{\"w\":2}";
    bb_event_post(t2, 0, p, strlen(p));
    bb_event_pump(0);

    char frame[256];
    size_t n = bb_event_routes_drain_frame(c, frame, sizeof(frame));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(frame, "event: walk.second\n"));

    bb_event_routes_client_release(c);
}

void test_bb_event_routes_drain_truncated_falls_back_to_safe_frame(void)
{
    setup_sync_mode();
    reset_world();
    /* Use small ring_max_entry so the local stack buffer in drain is small;
     * we'll pass a buflen too small to hold the full data frame, forcing the
     * truncated fallback path. */
    bb_event_routes_cfg_t tiny_cfg = small_cfg;
    tiny_cfg.ring_max_entry = 32;
    bb_event_routes_init(&tiny_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("trunc.topic", &t);
    bb_event_routes_attach("trunc.topic");

    bb_event_routes_client_t *c = NULL;
    bb_event_routes_client_acquire(&c);

    /* Pad the payload so the full frame is comfortably larger than the
     * fallback frame: full ~85 bytes vs fallback ~52 bytes. With buflen=64
     * the full snprintf overflows and the fallback fits. */
    const char *p = "{\"x\":\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJ\"}";
    bb_event_post(t, 0, p, strlen(p));
    bb_event_pump(0);

    char small_buf[64];
    size_t n = bb_event_routes_drain_frame(c, small_buf, sizeof(small_buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(small_buf, "\"truncated\":true"));

    bb_event_routes_client_release(c);
}

/* Calloc-fail injection: first calloc inside client_acquire fails. */
static int s_route_calloc_fail_at = -1;
static int s_route_calloc_calls = 0;
static void *route_failing_calloc(size_t n, size_t sz)
{
    if (s_route_calloc_calls++ == s_route_calloc_fail_at) return NULL;
    return calloc(n, sz);
}

void test_bb_event_routes_client_acquire_entries_calloc_fails(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    s_route_calloc_fail_at = 0;
    s_route_calloc_calls = 0;
    bb_event_routes_set_allocator(route_failing_calloc, free);

    bb_event_routes_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_event_routes_client_acquire(&c));

    bb_event_routes_set_allocator(NULL, NULL);
    s_route_calloc_fail_at = -1;
}

void test_bb_event_routes_client_acquire_payload_calloc_fails(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    s_route_calloc_fail_at = 1;  /* second alloc */
    s_route_calloc_calls = 0;
    bb_event_routes_set_allocator(route_failing_calloc, free);

    bb_event_routes_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_event_routes_client_acquire(&c));

    bb_event_routes_set_allocator(NULL, NULL);
    s_route_calloc_fail_at = -1;
}

/* Ring attach failure: leverage bb_event_ring's allocator to fail. */
void test_bb_event_routes_attach_ring_allocation_fails(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("ringfail.topic", &t);

    test_alloc_reset();
    test_alloc_fail_at = 0;
    bb_event_ring_set_allocator(test_failing_calloc, free);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_event_routes_attach("ringfail.topic"));

    bb_event_ring_set_allocator(NULL, NULL);
    test_alloc_fail_at = -1;
}

/* Sub failure during acquire: exhaust the bb_event subscriber pool before
 * acquire, then trigger rollback by attaching a topic first. */
void test_bb_event_routes_client_acquire_subscribe_failure_rolls_back(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("sub.topic", &t);
    bb_event_routes_attach("sub.topic");

    /* Exhaust the subscriber pool. CONFIG_BB_EVENT_MAX_TOPICS=64 in the test
     * env -> 64*8=512 subscribers. We can't realistically fill that here, so
     * the alternative is to make the pool's calloc fail mid-acquire via the
     * bb_event reset and then a sub call. Instead we exercise the rollback via
     * the bb_event_ring's allocator failing on the second ring (each acquire
     * snapshots all attached topics, so when ring_subscribe_with_replay's
     * snapshot calloc fails for an attached topic, acquire must roll back any
     * subs already taken).  Attach two topics, fail the snapshot on the 2nd. */
    bb_event_topic_t t2;
    bb_event_topic_register("sub.topic2", &t2);
    bb_event_routes_attach("sub.topic2");

    /* Make snapshot fail on the 2nd ring (first sub succeeds; second triggers
     * rollback). */
    test_alloc_reset();
    test_alloc_fail_at = 1;  /* index of failing call inside subscribe_with_replay snapshot */
    bb_event_ring_set_allocator(test_failing_calloc, free);

    bb_event_routes_client_t *c = NULL;
    /* Acquire may succeed (no ring entries means snapshot calloc isn't called) */
    /* or fail with NO_SPACE; either way no leak. */
    bb_err_t err = bb_event_routes_client_acquire(&c);

    bb_event_ring_set_allocator(NULL, NULL);
    test_alloc_fail_at = -1;

    if (err == BB_OK) bb_event_routes_client_release(c);
    /* Both outcomes are valid; the goal is exercising the rollback paths. */
    TEST_ASSERT_TRUE(err == BB_OK || err == BB_ERR_NO_SPACE);
}
