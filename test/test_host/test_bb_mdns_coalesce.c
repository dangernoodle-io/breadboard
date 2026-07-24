#include "unity.h"
#include "bb_mdns.h"
#include "bb_mdns_host_test_hooks.h"
#include "bb_mdns_test.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Shared state for per-peer callback tracking
 * ---------------------------------------------------------------------------*/

#define MAX_CAPTURED_PEERS 64

static int      s_peer_fired    = 0;
static int      s_removed_fired = 0;
static bb_mdns_peer_t s_captured[MAX_CAPTURED_PEERS];
static char           s_removed_names[MAX_CAPTURED_PEERS][BB_MDNS_INSTANCE_NAME_MAX];

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
    strncpy(p.id.instance_name, instance, BB_MDNS_INSTANCE_NAME_MAX - 1);
    strncpy(p.id.ip4, ip4 ? ip4 : "", BB_MDNS_IP4_MAX - 1);
    p.id.port = port;
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

    /* Append 6 peers (simulating a burst of peers from a single browse scan). */
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
        TEST_ASSERT_EQUAL_STRING(expected, s_captured[i].id.instance_name);
        TEST_ASSERT_EQUAL((uint16_t)(4000 + i), s_captured[i].id.port);
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
    TEST_ASSERT_EQUAL_STRING("solo-device", s_captured[0].id.instance_name);
    TEST_ASSERT_EQUAL(9000, s_captured[0].id.port);

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
 * TC6: 32 events back-to-back (no flush timer) → TWO batches of 16,
 *       all 32 peers preserved, all per-peer callbacks fired exactly once.
 *       This is the esp32-wroom32 single-core repro scenario.
 * ---------------------------------------------------------------------------*/
void test_coalesce_overflow_flush_32_events_two_batches(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", capture_peer_cb, NULL, NULL);

    /* Inject 32 peers without firing the flush timer.  At peer 17 the batch
     * overflows → synchronous flush of batch 1 (16 peers) → batch 2 starts. */
    for (int i = 0; i < 32; i++) {
        char name[BB_MDNS_INSTANCE_NAME_MAX];
        char ip[BB_MDNS_IP4_MAX];
        snprintf(name, sizeof(name), "acme-flood-%02d", i);
        snprintf(ip,   sizeof(ip),   "10.0.1.%d", i);
        bb_mdns_peer_t peer = make_peer(name, ip, (uint16_t)(5000 + i));
        bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &peer, false);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    /* After 32 appends: first 16 were auto-flushed (1 enqueue + 16 callbacks),
     * remaining 16 are still in the batch. */
    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(16, s_peer_fired);
    TEST_ASSERT_EQUAL(16, bb_mdns_coalesce_batch_count());

    /* Simulate flush timer for the second batch. */
    int dispatched = bb_mdns_coalesce_flush_for_test();
    TEST_ASSERT_EQUAL(16, dispatched);
    TEST_ASSERT_EQUAL(2, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(32, s_peer_fired);
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_batch_count());

    /* All 32 peers arrived in order (first 16 from auto-flush, next 16 from timer). */
    for (int i = 0; i < 32; i++) {
        char expected[BB_MDNS_INSTANCE_NAME_MAX];
        snprintf(expected, sizeof(expected), "acme-flood-%02d", i);
        TEST_ASSERT_EQUAL_STRING(expected, s_captured[i].id.instance_name);
        TEST_ASSERT_EQUAL((uint16_t)(5000 + i), s_captured[i].id.port);
    }

    bb_mdns_browse_stop("_acme", "_tcp");
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

/* ---------------------------------------------------------------------------
 * TC11: Queue-full path — timer flush rejected when queue depth is held at cap.
 *        Verify: 0 enqueues, 0 callbacks, batch INTACT (not cleared — the
 *        50 ms retry must re-attempt the SAME batch); drain recovers all peers.
 * ---------------------------------------------------------------------------*/
void test_coalesce_queue_full_flush_drops_and_drain_recovers(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", capture_peer_cb, NULL, NULL);

    /* Hold queue at cap (simulate full dispatcher queue). */
    bb_mdns_coalesce_queue_depth_cap_set_for_test(1);
    bb_mdns_coalesce_queue_depth_hold_for_test(1);

    /* Fill batch with 6 peers and timer-flush — enqueue rejected (queue full). */
    for (int i = 0; i < 6; i++) {
        char name[BB_MDNS_INSTANCE_NAME_MAX];
        snprintf(name, sizeof(name), "blocked-%02d", i);
        bb_mdns_peer_t p = make_peer(name, "10.2.0.1", 80);
        bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &p, false);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    bb_mdns_coalesce_flush_for_test();

    /* Flush was rejected: no enqueue, no callbacks. */
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(0, s_peer_fired);
    /* Batch must still be intact — the retry must see the same 6 entries. */
    TEST_ASSERT_EQUAL(6, bb_mdns_coalesce_batch_count());

    /* Drain the queue; retry flush delivers all 6 batched peers. */
    bb_mdns_coalesce_queue_drain_for_test();
    bb_mdns_coalesce_flush_for_test();

    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(6, s_peer_fired);
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_batch_count());
    for (int i = 0; i < 6; i++) {
        char expected[BB_MDNS_INSTANCE_NAME_MAX];
        snprintf(expected, sizeof(expected), "blocked-%02d", i);
        TEST_ASSERT_EQUAL_STRING(expected, s_captured[i].id.instance_name);
    }

    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * TC12: Queue-full overflow drop — BB_ERR_NO_SPACE when both batch and
 *        dispatcher queue are full simultaneously.
 *        Verify drop_count increments; batch stays intact; drain recovers
 *        all 16 buffered events (not just the overflow event).
 *
 * Under the corrected semantics a failed overflow flush does NOT clear the
 * batch — the 16 flood events survive and are delivered when the queue drains.
 * Only the 17th event (which triggered the failed flush) is dropped.
 * ---------------------------------------------------------------------------*/
void test_coalesce_queue_and_batch_full_drops_and_recovers(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", capture_peer_cb, NULL, NULL);

    /* Hold queue full (cap=1, depth=1). */
    bb_mdns_coalesce_queue_depth_cap_set_for_test(1);
    bb_mdns_coalesce_queue_depth_hold_for_test(1);

    /* Fill batch to MAX (16). */
    for (int i = 0; i < 16; i++) {
        char name[BB_MDNS_INSTANCE_NAME_MAX];
        snprintf(name, sizeof(name), "flood-%02d", i);
        bb_mdns_peer_t p = make_peer(name, "10.4.0.1", 80);
        bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &p, false);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_drop_count());

    /* 17th append triggers overflow flush → flush rejected (ring full) →
     * new event cannot be appended → BB_ERR_NO_SPACE returned to caller. */
    bb_mdns_peer_t extra = make_peer("overflow-event", "10.4.0.1", 80);
    bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &extra, false);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_drop_count());
    TEST_ASSERT_EQUAL(0, s_peer_fired);
    /* Batch must still hold the original 16 entries — not silently dropped. */
    TEST_ASSERT_EQUAL(16, bb_mdns_coalesce_batch_count());

    /* Drain the queue; a retry flush now delivers all 16 buffered flood events. */
    bb_mdns_coalesce_queue_drain_for_test();
    bb_mdns_coalesce_flush_for_test();
    TEST_ASSERT_EQUAL(1, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(16, s_peer_fired);
    TEST_ASSERT_EQUAL(0, bb_mdns_coalesce_batch_count());

    /* A subsequent ok_peer appends and flushes cleanly (second enqueue). */
    bb_mdns_peer_t ok_peer = make_peer("ok-peer", "10.5.0.1", 7777);
    err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &ok_peer, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    bb_mdns_coalesce_flush_for_test();
    TEST_ASSERT_EQUAL(2, bb_mdns_coalesce_queue_enqueue_count());
    TEST_ASSERT_EQUAL(17, s_peer_fired);
    TEST_ASSERT_EQUAL_STRING("ok-peer", s_captured[16].id.instance_name);

    bb_mdns_browse_stop("_acme", "_tcp");
}

/* ---------------------------------------------------------------------------
 * TC9 (regression for #311): TXT key/value pointers must remain valid in the
 * dispatched peer after the caller's source memory is overwritten. The
 * coalescing struct-copy previously left txt[i].key/value pointing into the
 * source evt's payload; once the source went out of scope the dispatcher
 * delivered dangling pointers. Verified by stomping the source buffers
 * AFTER append + BEFORE flush, then asserting the captured peer's TXT
 * data still reads the original strings.
 * ---------------------------------------------------------------------------*/
void test_coalesce_txt_pointers_survive_source_clobber(void)
{
    coalesce_setUp();
    bb_mdns_browse_start("_acme", "_tcp", capture_peer_cb, NULL, NULL);

    /* Caller-owned TXT storage that we'll deliberately clobber after append. */
    char k0[16] = "version";
    char v0[32] = "1.2.3-dev";
    char k1[16] = "board";
    char v1[32] = "tdongle-s3";
    bb_mdns_txt_t txts[2] = {
        { .key = k0, .value = v0 },
        { .key = k1, .value = v1 },
    };

    bb_mdns_peer_t peer = make_peer("txt-peer", "10.6.0.1", 5555);
    peer.txt       = txts;
    peer.txt_count = 2;

    bb_err_t err = bb_mdns_coalesce_append_for_test("_acme", "_tcp", &peer, false);
    TEST_ASSERT_EQUAL(BB_OK, err);

    /* Source memory goes away — overwrite everything the caller owned. */
    memset(k0,   0xAA, sizeof(k0));
    memset(v0,   0xAA, sizeof(v0));
    memset(k1,   0xAA, sizeof(k1));
    memset(v1,   0xAA, sizeof(v1));
    memset(txts, 0xAA, sizeof(txts));
    memset(&peer, 0xAA, sizeof(peer));

    /* Flush; the dispatcher must see the ORIGINAL TXT strings. */
    bb_mdns_coalesce_flush_for_test();
    TEST_ASSERT_EQUAL(1, s_peer_fired);
    TEST_ASSERT_EQUAL(2, s_captured[0].txt_count);
    TEST_ASSERT_EQUAL_STRING("version",   s_captured[0].txt[0].key);
    TEST_ASSERT_EQUAL_STRING("1.2.3-dev", s_captured[0].txt[0].value);
    TEST_ASSERT_EQUAL_STRING("board",     s_captured[0].txt[1].key);
    TEST_ASSERT_EQUAL_STRING("tdongle-s3", s_captured[0].txt[1].value);

    bb_mdns_browse_stop("_acme", "_tcp");
}
