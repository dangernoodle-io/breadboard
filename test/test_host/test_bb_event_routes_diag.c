#include "unity.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"
#include "bb_event_test.h"
#include <stdlib.h>
#include <string.h>

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
    .max_clients     = 2,
    .per_client_queue = 4,
    .ring_capacity   = 4,
    .ring_max_entry  = 64,
    .heartbeat_ms    = 1000,
};

// ---------------------------------------------------------------------------
// bb_event_routes_topic_count
// ---------------------------------------------------------------------------

void test_bb_event_routes_topic_count_zero_before_attach(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    TEST_ASSERT_EQUAL(0, bb_event_routes_topic_count());
}

void test_bb_event_routes_topic_count_increments_on_attach(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t1, t2;
    bb_event_topic_register("diag.t1", &t1);
    bb_event_topic_register("diag.t2", &t2);

    bb_event_routes_attach("diag.t1");
    TEST_ASSERT_EQUAL(1, bb_event_routes_topic_count());

    bb_event_routes_attach("diag.t2");
    TEST_ASSERT_EQUAL(2, bb_event_routes_topic_count());
}

void test_bb_event_routes_topic_count_unchanged_on_dedup_attach(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("diag.dedup", &t);
    bb_event_routes_attach("diag.dedup");
    bb_event_routes_attach("diag.dedup");  // idempotent

    TEST_ASSERT_EQUAL(1, bb_event_routes_topic_count());
}

// ---------------------------------------------------------------------------
// bb_event_routes_topic_info
// ---------------------------------------------------------------------------

void test_bb_event_routes_topic_info_out_of_range_returns_not_found(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    const char *name = NULL;
    bb_event_ring_t ring = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_event_routes_topic_info(0, &name, &ring));
}

void test_bb_event_routes_topic_info_returns_correct_name(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("diag.name.test", &t);
    bb_event_routes_attach("diag.name.test");

    const char *name = NULL;
    bb_event_ring_t ring = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_topic_info(0, &name, &ring));
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING("diag.name.test", name);
    TEST_ASSERT_NOT_NULL(ring);
}

void test_bb_event_routes_topic_info_null_out_params_ok(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("diag.null.out", &t);
    bb_event_routes_attach("diag.null.out");

    // Both out-params NULL — should not crash.
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_topic_info(0, NULL, NULL));
}

void test_bb_event_routes_topic_info_multiple_topics(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t1, t2;
    bb_event_topic_register("diag.multi.a", &t1);
    bb_event_topic_register("diag.multi.b", &t2);
    bb_event_routes_attach("diag.multi.a");
    bb_event_routes_attach("diag.multi.b");

    const char *name0 = NULL, *name1 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_topic_info(0, &name0, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_topic_info(1, &name1, NULL));
    TEST_ASSERT_NOT_NULL(name0);
    TEST_ASSERT_NOT_NULL(name1);
    TEST_ASSERT_EQUAL_STRING("diag.multi.a", name0);
    TEST_ASSERT_EQUAL_STRING("diag.multi.b", name1);
}

void test_bb_event_routes_topic_info_ring_reflects_posts(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("diag.ring.reflect", &t);
    bb_event_routes_attach("diag.ring.reflect");

    bb_event_ring_t ring = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_topic_info(0, NULL, &ring));
    TEST_ASSERT_NOT_NULL(ring);

    // Ring is empty before any posts.
    TEST_ASSERT_EQUAL(0, bb_event_ring_count(ring));

    const char *payload = "{\"k\":1}";
    bb_event_post(t, 42, payload, strlen(payload));
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(1, bb_event_ring_count(ring));

    uint32_t id = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_ring_last_entry_info(ring, &id, NULL, NULL));
    TEST_ASSERT_EQUAL(42, id);
}

// ---------------------------------------------------------------------------
// bb_event_routes_active_client_count
// ---------------------------------------------------------------------------

void test_bb_event_routes_active_client_count_zero_before_acquire(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    TEST_ASSERT_EQUAL(0, bb_event_routes_active_client_count());
}

void test_bb_event_routes_active_client_count_increments_on_acquire(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_routes_client_t *c1 = NULL, *c2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_client_acquire(&c1));
    TEST_ASSERT_EQUAL(1, bb_event_routes_active_client_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_client_acquire(&c2));
    TEST_ASSERT_EQUAL(2, bb_event_routes_active_client_count());

    bb_event_routes_client_release(c1);
    TEST_ASSERT_EQUAL(1, bb_event_routes_active_client_count());

    bb_event_routes_client_release(c2);
    TEST_ASSERT_EQUAL(0, bb_event_routes_active_client_count());
}

void test_bb_event_routes_active_client_count_at_max(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);  /* max_clients=2 */

    bb_event_routes_client_t *c1 = NULL, *c2 = NULL, *c3 = NULL;
    bb_event_routes_client_acquire(&c1);
    bb_event_routes_client_acquire(&c2);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_event_routes_client_acquire(&c3));
    TEST_ASSERT_EQUAL(2, bb_event_routes_active_client_count());

    bb_event_routes_client_release(c1);
    bb_event_routes_client_release(c2);
}

// ---------------------------------------------------------------------------
// Combined: topic_info ring state matches what last_entry_info returns
// ---------------------------------------------------------------------------

void test_bb_event_routes_diag_full_round_trip(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t ta, tb;
    bb_event_topic_register("diag.rt.a", &ta);
    bb_event_topic_register("diag.rt.b", &tb);
    bb_event_routes_attach("diag.rt.a");
    bb_event_routes_attach("diag.rt.b");

    TEST_ASSERT_EQUAL(2, bb_event_routes_topic_count());

    // Post to topic a only.
    const char *json = "{\"hello\":\"world\"}";
    bb_event_post(ta, 77, json, strlen(json));
    bb_event_pump(0);

    // topic index 0 -> "diag.rt.a"
    bb_event_ring_t ring_a = NULL;
    const char *name_a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_topic_info(0, &name_a, &ring_a));
    TEST_ASSERT_EQUAL_STRING("diag.rt.a", name_a);
    TEST_ASSERT_EQUAL(1, bb_event_ring_count(ring_a));

    uint32_t id = 0;
    size_t sz = 0;
    int64_t us = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_ring_last_entry_info(ring_a, &id, &sz, &us));
    TEST_ASSERT_EQUAL(77, id);
    TEST_ASSERT_EQUAL(strlen(json), sz);
    TEST_ASSERT_TRUE(us > 0);

    // topic index 1 -> "diag.rt.b" — ring is empty.
    bb_event_ring_t ring_b = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_routes_topic_info(1, NULL, &ring_b));
    TEST_ASSERT_EQUAL(0, bb_event_ring_count(ring_b));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_event_ring_last_entry_info(ring_b, NULL, NULL, NULL));

    // No clients active.
    TEST_ASSERT_EQUAL(0, bb_event_routes_active_client_count());
}

// ---------------------------------------------------------------------------
// Edge: topic_info after reset
// ---------------------------------------------------------------------------

void test_bb_event_routes_topic_count_zero_after_reset(void)
{
    setup_sync_mode();
    reset_world();
    bb_event_routes_init(&small_cfg);

    bb_event_topic_t t;
    bb_event_topic_register("diag.reset.t", &t);
    bb_event_routes_attach("diag.reset.t");
    TEST_ASSERT_EQUAL(1, bb_event_routes_topic_count());

    bb_event_routes_reset_for_test();
    TEST_ASSERT_EQUAL(0, bb_event_routes_topic_count());
}
