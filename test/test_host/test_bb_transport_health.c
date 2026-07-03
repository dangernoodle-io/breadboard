#include "unity.h"
#include "bb_transport_health.h"

static void reset_world(void)
{
    bb_transport_health_reset_for_test();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void test_bb_transport_health_register_authoritative(void)
{
    reset_world();
    bb_transport_handle_t h = BB_TRANSPORT_HANDLE_INVALID;
    bb_err_t rc = bb_transport_health_register("mqtt", BB_TRANSPORT_AUTHORITATIVE, &h);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NOT_EQUAL(BB_TRANSPORT_HANDLE_INVALID, h);
}

void test_bb_transport_health_register_inferred(void)
{
    reset_world();
    bb_transport_handle_t h = BB_TRANSPORT_HANDLE_INVALID;
    bb_err_t rc = bb_transport_health_register("stratum", BB_TRANSPORT_INFERRED, &h);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NOT_EQUAL(BB_TRANSPORT_HANDLE_INVALID, h);
}

void test_bb_transport_health_register_capacity_exhausted(void)
{
    reset_world();
    bb_transport_handle_t h;
    for (int i = 0; i < BB_TRANSPORT_HEALTH_MAX_SLOTS; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_register("t", BB_TRANSPORT_AUTHORITATIVE, &h));
    }
    h = 42; // sentinel, must be overwritten with BB_TRANSPORT_HANDLE_INVALID
    bb_err_t rc = bb_transport_health_register("overflow", BB_TRANSPORT_AUTHORITATIVE, &h);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL(BB_TRANSPORT_HANDLE_INVALID, h);
}

void test_bb_transport_health_register_invalid_class_rejected(void)
{
    reset_world();
    bb_transport_handle_t h = 42; // sentinel
    bb_err_t rc = bb_transport_health_register("bogus", (bb_transport_class_t)99, &h);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
    TEST_ASSERT_EQUAL(BB_TRANSPORT_HANDLE_INVALID, h);

    // Slot must not have been consumed — a subsequent valid register still succeeds.
    bb_transport_handle_t good;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_register("mqtt", BB_TRANSPORT_AUTHORITATIVE, &good));

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL(1, enabled);
}

void test_bb_transport_health_register_null_args(void)
{
    reset_world();
    bb_transport_handle_t h;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_transport_health_register(NULL, BB_TRANSPORT_AUTHORITATIVE, &h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_transport_health_register("x", BB_TRANSPORT_AUTHORITATIVE, NULL));
}

// ---------------------------------------------------------------------------
// report() — AUTHORITATIVE only
// ---------------------------------------------------------------------------

void test_bb_transport_health_report_ok_clears_failing(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("mqtt", BB_TRANSPORT_AUTHORITATIVE, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_report(h, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_report(h, true));

    bb_transport_health_snapshot_t snap[1];
    TEST_ASSERT_EQUAL(1, bb_transport_health_snapshot_all(snap, 1));
    TEST_ASSERT_FALSE(snap[0].failing);
    TEST_ASSERT_EQUAL_UINT32(1, snap[0].fail_count);
}

void test_bb_transport_health_report_fail_sets_failing_and_bumps_count(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("mqtt", BB_TRANSPORT_AUTHORITATIVE, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_report(h, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_report(h, false));

    bb_transport_health_snapshot_t snap[1];
    TEST_ASSERT_EQUAL(1, bb_transport_health_snapshot_all(snap, 1));
    TEST_ASSERT_TRUE(snap[0].failing);
    TEST_ASSERT_EQUAL_UINT32(2, snap[0].fail_count);
}

void test_bb_transport_health_report_wrong_class_rejected(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("stratum", BB_TRANSPORT_INFERRED, &h);
    bb_err_t rc = bb_transport_health_report(h, true);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

void test_bb_transport_health_report_invalid_handle(void)
{
    reset_world();
    bb_err_t rc = bb_transport_health_report(99, true);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

// ---------------------------------------------------------------------------
// mark_activity() — INFERRED only
// ---------------------------------------------------------------------------

void test_bb_transport_health_mark_activity_bumps_rx_count(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("stratum", BB_TRANSPORT_INFERRED, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_mark_activity(h));
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_mark_activity(h));

    bb_transport_health_snapshot_t snap[1];
    TEST_ASSERT_EQUAL(1, bb_transport_health_snapshot_all(snap, 1));
    TEST_ASSERT_EQUAL_UINT32(2, snap[0].rx_count);
}

void test_bb_transport_health_mark_activity_wrong_class_rejected(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("mqtt", BB_TRANSPORT_AUTHORITATIVE, &h);
    bb_err_t rc = bb_transport_health_mark_activity(h);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

// ---------------------------------------------------------------------------
// set_enabled()
// ---------------------------------------------------------------------------

void test_bb_transport_health_set_enabled_excludes_from_counts(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("mqtt", BB_TRANSPORT_AUTHORITATIVE, &h);
    bb_transport_health_set_enabled(h, false);

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL(0, enabled);
    TEST_ASSERT_EQUAL(0, failing);
}

void test_bb_transport_health_set_enabled_invalid_handle(void)
{
    reset_world();
    bb_err_t rc = bb_transport_health_set_enabled(99, false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

// ---------------------------------------------------------------------------
// authoritative_counts() — the observe-only invariant
// ---------------------------------------------------------------------------

void test_bb_transport_health_authoritative_counts_basic(void)
{
    reset_world();
    bb_transport_handle_t a, b;
    bb_transport_health_register("mqtt", BB_TRANSPORT_AUTHORITATIVE, &a);
    bb_transport_health_register("http", BB_TRANSPORT_AUTHORITATIVE, &b);
    bb_transport_health_report(a, false);

    int enabled = 0, failing = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL(2, enabled);
    TEST_ASSERT_EQUAL(1, failing);
}

// Observe-only invariant: an INFERRED slot never contributes to
// authoritative_counts(), even when it would be considered "stale" at
// snapshot time. This is the structural guarantee the component exists for.
void test_bb_transport_health_authoritative_counts_never_counts_inferred(void)
{
    reset_world();
    bb_transport_handle_t inferred;
    bb_transport_health_register("stratum", BB_TRANSPORT_INFERRED, &inferred);
    // No activity ever marked — this slot is maximally "stale".

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL(0, enabled);
    TEST_ASSERT_EQUAL(0, failing);
}

void test_bb_transport_health_authoritative_counts_null_args(void)
{
    reset_world();
    int enabled;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_transport_health_authoritative_counts(NULL, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_transport_health_authoritative_counts(&enabled, NULL));
}

// ---------------------------------------------------------------------------
// snapshot_all()
// ---------------------------------------------------------------------------

void test_bb_transport_health_snapshot_all_cap_truncation(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("a", BB_TRANSPORT_AUTHORITATIVE, &h);
    bb_transport_health_register("b", BB_TRANSPORT_AUTHORITATIVE, &h);
    bb_transport_health_register("c", BB_TRANSPORT_AUTHORITATIVE, &h);

    bb_transport_health_snapshot_t snap[2];
    size_t n = bb_transport_health_snapshot_all(snap, 2);
    TEST_ASSERT_EQUAL(2, n);
}

void test_bb_transport_health_snapshot_all_inferred_failing_via_staleness(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("stratum", BB_TRANSPORT_INFERRED, &h);
    // Never marked active -> last_rx_ms stays 0, so vs any now_ms > threshold
    // it reads as stale/failing at snapshot time.
    bb_transport_health_snapshot_t snap[1];
    size_t n = bb_transport_health_snapshot_all(snap, 1);
    TEST_ASSERT_EQUAL(1, n);
    // last_rx_ms == 0 and now_ms is real wall time (large), so this is stale.
    TEST_ASSERT_TRUE(snap[0].failing);
}

void test_bb_transport_health_snapshot_all_null_or_zero_max(void)
{
    reset_world();
    bb_transport_handle_t h;
    bb_transport_health_register("a", BB_TRANSPORT_AUTHORITATIVE, &h);
    bb_transport_health_snapshot_t snap[1];
    TEST_ASSERT_EQUAL(0, bb_transport_health_snapshot_all(NULL, 1));
    TEST_ASSERT_EQUAL(0, bb_transport_health_snapshot_all(snap, 0));
}

// ---------------------------------------------------------------------------
// is_stale() — boundary
// ---------------------------------------------------------------------------

void test_bb_transport_health_is_stale_below_threshold(void)
{
    TEST_ASSERT_FALSE(bb_transport_health_is_stale(0, 59000, 60));
}

void test_bb_transport_health_is_stale_at_threshold(void)
{
    TEST_ASSERT_FALSE(bb_transport_health_is_stale(0, 60000, 60));
}

void test_bb_transport_health_is_stale_above_threshold(void)
{
    TEST_ASSERT_TRUE(bb_transport_health_is_stale(0, 60001, 60));
}

void test_bb_transport_health_is_stale_now_before_last_rx(void)
{
    // Clock went backwards / same tick — never stale.
    TEST_ASSERT_FALSE(bb_transport_health_is_stale(1000, 500, 60));
}

void test_bb_transport_health_is_stale_zero_threshold_same_ms(void)
{
    // now == last_rx, threshold 0: diff == 0, not > 0 -> not stale.
    TEST_ASSERT_FALSE(bb_transport_health_is_stale(1000, 1000, 0));
}

void test_bb_transport_health_is_stale_zero_threshold_one_ms_elapsed(void)
{
    // now > last_rx by 1ms, threshold 0: diff == 1 > 0 -> stale.
    TEST_ASSERT_TRUE(bb_transport_health_is_stale(1000, 1001, 0));
}
