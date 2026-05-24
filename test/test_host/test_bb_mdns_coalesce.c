#include "unity.h"
#include "bb_mdns.h"
#include "bb_mdns_host_test_hooks.h"
#include "bb_mdns_test.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Shared state for per-peer callback tracking
 * ---------------------------------------------------------------------------*/

#define MAX_CAPTURED_PEERS 16

static int      s_peer_fired    = 0;
static int      s_removed_fired = 0;
static bb_mdns_peer_t s_captured[MAX_CAPTURED_PEERS];
static char     s_removed_names[MAX_CAPTURED_PEERS][BB_MDNS_INSTANCE_NAME_MAX];

static void capture_peer_cb(const bb_mdns_peer_t *peer, void *ctx)
{
    (void)ctx;
    if (s_peer_fired < MAX_CAPTURED_PEERS) {
        s_captured[s_peer_fired] = *peer;
    }
    s_peer_fired++;
}

static void capture_removed_cb(const char *instance_name, void *ctx)
{
    (void)ctx;
    if (s_removed_fired < MAX_CAPTURED_PEERS) {
        strncpy(s_removed_names[s_removed_fired], instance_name ? instance_name : "",
                BB_MDNS_INSTANCE_NAME_MAX - 1);
        s_removed_names[s_removed_fired][BB_MDNS_INSTANCE_NAME_MAX - 1] = '\0';
    }
    s_removed_fired++;
}

static void reset_captured(void)
{
    s_peer_fired    = 0;
    s_removed_fired = 0;
    memset(s_captured, 0, sizeof(s_captured));
    memset(s_removed_names, 0, sizeof(s_removed_names));
}

/* setUp / tearDown called by Unity per-test */
static void coalesce_setUp(void)
{
    bb_mdns_host_reset();
    bb_mdns_coalesce_reset_for_test();
    reset_captured();
    /* Ensure a clean subscription table for each test. */
    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * Helper: build a minimal peer struct
 * ---------------------------------------------------------------------------*/
static bb_mdns_peer_t make_peer(const char *instance, const char *ip4, uint16_t port)
{
    bb_mdns_peer_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.instance_name, instance, BB_MDNS_INSTANCE_NAME_MAX - 1);
    strncpy(p.ip4, ip4 ? ip4 : "", BB_MDNS_IP4_MAX - 1);
    p.port = port;
    return p;
}

/* ---------------------------------------------------------------------------
 * TC1: N peers appended to batch, single flush enqueues ONE queue item,
 *       all N per-peer callbacks fire exactly once, in order.
 * ---------------------------------------------------------------------------*/
void test_coalesce_n_peers_single_flush(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", capture_peer_cb, NULL, NULL);

    /* Append 6 peers (simulating a browse-refresh burst). */
    for (int i = 0; i < 6; i++) {
        char name[BB_MDNS_INSTANCE_NAME_MAX];
        char ip[BB_MDNS_IP4_MAX];
        snprintf(name, sizeof(name), "acme-device-%02d", i);
        snprintf(ip,   sizeof(ip),   "192.168.1.%d", 10 + i);
        bb_mdns_peer_t peer = make_peer(name, ip, (uint16_t)(4000 + i));
        bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &peer, false);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    /* Before flush: batch has 6 entries, nothing dispatched yet. */
    TEST_ASSERT_EQUAL(6, bb_mdns_coalesce_batch_count());
    TEST_ASSERT_EQUAL(0, s_peer_fired);

    /* Simulate timer fire (flush). */
    int dispatched = bb_mdns_coalesce_flush_for_test();

    /* (a) single batched enqueue */
    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_queue_enqueue_count());
    /* (b) all 6 peers dispatched */
    TEST_ASSERT_EQUAL(6, dispatched);
    TEST_ASSERT_EQUAL(6, s_peer_fired);
    /* (c) batch is now empty */
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_batch_count());
    /* (d) peers ordered and present */
    for (int i = 0; i < 6; i++) {
        char expected[BB_MDNS_INSTANCE_NAME_MAX];
        snprintf(expected, sizeof(expected), "acme-device-%02d", i);
        TEST_ASSERT_EQUAL_STRING(expected, s_captured[i].instance_name);
        TEST_ASSERT_EQUAL((uint16_t)(4000 + i), s_captured[i].port);
    }

    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * TC2: Single-peer case — flush still happens, latency capped at window.
 * ---------------------------------------------------------------------------*/
void test_coalesce_single_peer_flush_fires(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", capture_peer_cb, NULL, NULL);

    bb_mdns_peer_t peer = make_peer("solo-device", "10.0.0.1", 9000);
    bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &peer, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_batch_count());
    TEST_ASSERT_EQUAL(0, s_peer_fired);

    int dispatched = bb_mdns_coalesce_flush_for_test();

    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(1, dispatched);
    TEST_ASSERT_EQUAL(1, s_peer_fired);
    TEST_ASSERT_EQUAL_STRING("solo-device", s_captured[0].instance_name);
    TEST_ASSERT_EQUAL(9000, s_captured[0].port);

    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * TC3: Removal events coalesce and dispatch via on_removed callback.
 * ---------------------------------------------------------------------------*/
void test_coalesce_removal_events_dispatch(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", NULL, capture_removed_cb, NULL);

    /* Append 3 removal events. */
    for (int i = 0; i < 3; i++) {
        char name[BB_MDNS_INSTANCE_NAME_MAX];
        snprintf(name, sizeof(name), "acme-gone-%02d", i);
        bb_mdns_peer_t peer = make_peer(name, "", 0);
        bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &peer, true);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    int dispatched = bb_mdns_coalesce_flush_for_test();

    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(3, dispatched);
    TEST_ASSERT_EQUAL(3, s_removed_fired);
    for (int i = 0; i < 3; i++) {
        char expected[BB_MDNS_INSTANCE_NAME_MAX];
        snprintf(expected, sizeof(expected), "acme-gone-%02d", i);
        TEST_ASSERT_EQUAL_STRING(expected, s_removed_names[i]);
    }

    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * TC4: Flush with empty batch is a no-op (0 enqueues, 0 dispatched).
 * ---------------------------------------------------------------------------*/
void test_coalesce_flush_empty_batch_is_noop(void)
{
    coalesce_setUp();

    int dispatched = bb_mdns_coalesce_flush_for_test();

    TEST_ASSERT_EQUAL(0, dispatched);
    /* Flush was called once but produced no queue enqueue. */
    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_flush_count());
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_queue_enqueue_count());
}

/* ---------------------------------------------------------------------------
 * TC5: Concurrent-append-during-flush contract.
 *       Simulate: flush drains batch A, a new append starts batch B,
 *       a second flush drains batch B.  Each flush produces exactly
 *       one queue enqueue.
 * ---------------------------------------------------------------------------*/
void test_coalesce_concurrent_append_goes_into_next_batch(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", capture_peer_cb, NULL, NULL);

    /* Batch A: 2 peers */
    bb_mdns_peer_t pa = make_peer("batch-a-peer-0", "192.168.2.1", 1000);
    bb_mdns_peer_t pb = make_peer("batch-a-peer-1", "192.168.2.2", 1001);
    bb_mdns_coalesce_append_for_test("_acme", "_tcp", &pa, false);
    bb_mdns_coalesce_append_for_test("_acme", "_tcp", &pb, false);

    /* Flush batch A. */
    int d1 = bb_mdns_coalesce_flush_for_test();
    TEST_ASSERT_EQUAL(2, d1);
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_batch_count());

    /* "Concurrent" append during flush — batch B starts immediately after. */
    bb_mdns_peer_t pc = make_peer("batch-b-peer-0", "192.168.3.1", 2000);
    bb_mdns_coalesce_append_for_test("_acme", "_tcp", &pc, false);
    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_batch_count());

    /* Flush batch B — second enqueue. */
    int d2 = bb_mdns_coalesce_flush_for_test();
    TEST_ASSERT_EQUAL(1, d2);
    TEST_ASSERT_EQUAL(2, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(3, s_peer_fired);  /* 2 + 1 */

    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * TC6: Batch full returns BB_ERR_NO_SPACE (overflow guard).
 * ---------------------------------------------------------------------------*/
void test_coalesce_batch_full_returns_no_space(void)
{
    coalesce_setUp();

    /* Fill the batch to capacity (BB_MDNS_HOST_BATCH_MAX == 16). */
    for (int i = 0; i < 16; i++) {
        char name[BB_MDNS_INSTANCE_NAME_MAX];
        snprintf(name, sizeof(name), "acme-flood-%02d", i);
        bb_mdns_peer_t peer = make_peer(name, "1.2.3.4", 80);
        bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &peer, false);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    /* 17th append must fail. */
    bb_mdns_peer_t overflow = make_peer("overflow-peer", "1.2.3.4", 80);
    bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &overflow, false);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

/* ---------------------------------------------------------------------------
 * TC7: Multiple sequential flushes each produce exactly one enqueue.
 * ---------------------------------------------------------------------------*/
void test_coalesce_multiple_flushes_each_one_enqueue(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", capture_peer_cb, NULL, NULL);

    for (int flush = 0; flush < 3; flush++) {
        bb_mdns_peer_t peer = make_peer("repeat-peer", "10.0.0.1", 80);
        bb_mdns_coalesce_append_for_test("_acme", "_tcp", &peer, false);
        bb_mdns_coalesce_flush_for_test();
    }

    TEST_ASSERT_EQUAL(3, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(3, s_peer_fired);

    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * TC8: Append with null peer (removal with no peer struct) is safe.
 * ---------------------------------------------------------------------------*/
void test_coalesce_append_null_peer_removal_is_safe(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", NULL, capture_removed_cb, NULL);

    /* NULL peer with is_removal=true — only instance name matters for removals.
     * The host shim uses the instance_name from the peer struct; with NULL peer
     * the instance name is empty, which is a valid (if degenerate) removal. */
    bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", NULL, true);
    TEST_ASSERT_EQUAL(BB_OK, err);

    int dispatched = bb_mdns_coalesce_flush_for_test();
    TEST_ASSERT_EQUAL(1, dispatched);
    TEST_ASSERT_EQUAL(1, s_removed_fired);

    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * TC9: No subscription — flush dispatches but no callback fires (no crash).
 * ---------------------------------------------------------------------------*/
void test_coalesce_flush_no_subscription_no_crash(void)
{
    coalesce_setUp();
    /* No browse_start. */

    bb_mdns_peer_t peer = make_peer("orphan-device", "10.0.0.1", 80);
    bb_mdns_coalesce_append_for_test("_nosub", "_tcp", &peer, false);

    int dispatched = bb_mdns_coalesce_flush_for_test();
    TEST_ASSERT_EQUAL(1, dispatched);
    TEST_ASSERT_EQUAL(0, s_peer_fired);
}

/* ---------------------------------------------------------------------------
 * TC10: reset clears flush and enqueue counters.
 * ---------------------------------------------------------------------------*/
void test_coalesce_reset_clears_all_state(void)
{
    coalesce_setUp();

    bb_mdns_peer_t peer = make_peer("some-device", "10.0.0.1", 80);
    bb_mdns_coalesce_append_for_test("_acme", "_tcp", &peer, false);
    bb_mdns_coalesce_flush_for_test();

    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_flush_count());
    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_queue_enqueue_count());

    bb_mdns_coalesce_reset_for_test();

    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_batch_count());
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_flush_count());
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_queue_enqueue_count());
}
