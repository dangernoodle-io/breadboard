// Tests for bb_wifi_http_diag -- exercises bb_wifi_http_diag_fill() (the
// exact production code path) against bb_wifi's BB_WIFI_TESTING host hooks.
// Scenario mirrors the retired test_route_fidelity.c h_diag_wifi fixture
// (B1-1077 PR-3a): reason_histogram present-gate coverage needs both a
// zero-histogram run (every bucket omitted) and a mixed run (some buckets
// present, some not) to exercise every present predicate's both branches.
#include "unity.h"
#include "bb_wifi_http_diag.h"
#include "bb_wifi.h"
#include "bb_wifi_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Minimal recording emit vtable -- drives bb_serialize_walk() over
// bb_wifi_http_diag_desc so every reason_histogram bucket's present
// predicate actually runs (bb_wifi_http_diag_fill() alone never calls the
// walker). Records every key seen; a bucket that is present-gated false
// never appears here.
// ---------------------------------------------------------------------------

#define REC_MAX_KEYS 32

static const char *s_rec_keys[REC_MAX_KEYS];
static size_t      s_rec_n;

static void rec_reset(void)
{
    s_rec_n = 0;
    memset(s_rec_keys, 0, sizeof(s_rec_keys));
}

static void rec_key(void *ctx, const char *key)
{
    (void)ctx;
    if (key && s_rec_n < REC_MAX_KEYS) s_rec_keys[s_rec_n++] = key;
}

static void rec_begin_obj(void *ctx, const char *key) { rec_key(ctx, key); }
static void rec_end_obj(void *ctx) { (void)ctx; }
static void rec_begin_arr(void *ctx, const char *key) { rec_key(ctx, key); }
static void rec_end_arr(void *ctx) { (void)ctx; }
static void rec_i64(void *ctx, const char *key, int64_t v) { (void)v; rec_key(ctx, key); }
static void rec_u64(void *ctx, const char *key, uint64_t v) { (void)v; rec_key(ctx, key); }
static void rec_f64(void *ctx, const char *key, double v) { (void)v; rec_key(ctx, key); }
static void rec_bool(void *ctx, const char *key, bool v) { (void)v; rec_key(ctx, key); }
static void rec_str(void *ctx, const char *key, const char *s, size_t len) { (void)s; (void)len; rec_key(ctx, key); }
static void rec_null(void *ctx, const char *key) { rec_key(ctx, key); }

static const bb_serialize_emit_t s_rec_emit = {
    .format_id = BB_FORMAT_NONE,
    .ctx       = NULL,
    .begin_obj = rec_begin_obj,
    .end_obj   = rec_end_obj,
    .begin_arr = rec_begin_arr,
    .end_arr   = rec_end_arr,
    .emit_i64  = rec_i64,
    .emit_u64  = rec_u64,
    .emit_f64  = rec_f64,
    .emit_bool = rec_bool,
    .emit_str  = rec_str,
    .emit_null = rec_null,
};

static bool rec_saw(const char *key)
{
    for (size_t i = 0; i < s_rec_n; i++) {
        if (s_rec_keys[i] && strcmp(s_rec_keys[i], key) == 0) return true;
    }
    return false;
}

// bb_wifi's own host default for disconnect_rssi is the INT8_MIN "no
// reading" sentinel (platform/host/bb_wifi/bb_wifi_host.c), not 0 -- restore
// THAT, not a bare 0, or a later test asserting the sentinel default (e.g.
// test_bb_wifi_disconnect_rssi_default_sentinel) would see stale state from
// this file's own bb_wifi_test_set_disconnect_rssi() calls.
static void wifi_diag_test_reset(void)
{
    bb_wifi_test_set_associated(false);
    bb_wifi_test_set_has_ip(false);
    bb_wifi_test_set_roam_count(0);
    bb_wifi_test_set_roam_age_s(0);
    bb_wifi_test_set_last_session_s(0);
    bb_wifi_test_set_restart_sta_count(0);
    bb_wifi_test_set_disconnect_rssi(INT8_MIN);
    uint16_t empty_hist[BB_WIFI_DISC_COUNT];
    memset(empty_hist, 0, sizeof(empty_hist));
    bb_wifi_test_set_reason_histogram(empty_hist, BB_WIFI_DISC_COUNT);
}

void test_bb_wifi_http_diag_fill_null_dst_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_wifi_http_diag_fill(NULL, NULL));
}

// Zero histogram: every reason_histogram bucket field's present predicate
// must return false (all omitted); top_reason/top_reason_count stay
// present with the zero-summary values.
void test_bb_wifi_http_diag_fill_empty_histogram_all_buckets_absent(void)
{
    wifi_diag_test_reset();

    bb_wifi_http_diag_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_http_diag_fill(&snap, NULL));

    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.unknown);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.assoc_leave);
    TEST_ASSERT_EQUAL_STRING("unknown", snap.histogram.top_reason.ptr);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.top_reason_count);

    wifi_diag_test_reset();
}

// Mixed histogram -- same scenario as the retired route-fidelity fixture:
// three breadboard-injected buckets plus one standard reason (the top
// non-injected count). Exercises the present=true branch for 4 buckets and
// present=false for the other 9.
void test_bb_wifi_http_diag_fill_mixed_histogram_and_shared_fields(void)
{
    wifi_diag_test_reset();
    bb_wifi_test_set_associated(true);
    bb_wifi_test_set_has_ip(true);
    bb_wifi_test_set_roam_count(2);
    bb_wifi_test_set_roam_age_s(45);
    bb_wifi_test_set_last_session_s(120);
    bb_wifi_test_set_restart_sta_count(3);
    bb_wifi_test_set_disconnect_rssi(-70);

    uint16_t hist[BB_WIFI_DISC_COUNT];
    memset(hist, 0, sizeof(hist));
    hist[BB_WIFI_DISC_BB_LOST_IP]        = 1;
    hist[BB_WIFI_DISC_BB_EGRESS_DEAD]    = 4;
    hist[BB_WIFI_DISC_BB_NO_IP_WATCHDOG] = 3;
    hist[BB_WIFI_DISC_INACTIVITY]        = 7;  // top non-injected reason
    bb_wifi_test_set_reason_histogram(hist, BB_WIFI_DISC_COUNT);

    bb_wifi_http_diag_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_http_diag_fill(&snap, NULL));

    TEST_ASSERT_TRUE(snap.associated);
    TEST_ASSERT_TRUE(snap.has_ip);
    TEST_ASSERT_EQUAL_STRING("ok", snap.net_mode.ptr);
    TEST_ASSERT_EQUAL_INT64(2, snap.roam_count);
    TEST_ASSERT_EQUAL_INT64(45, snap.roam_age_s);
    TEST_ASSERT_EQUAL_INT64(120, snap.last_session_s);
    TEST_ASSERT_EQUAL_INT64(3, snap.restart_sta_count);
    TEST_ASSERT_EQUAL_INT64(-70, snap.disconnect_rssi);
    TEST_ASSERT_EQUAL_STRING("00:00:00:00:00:00", snap.bssid);

    TEST_ASSERT_EQUAL_INT64(1, snap.histogram.bb_lost_ip);
    TEST_ASSERT_EQUAL_INT64(4, snap.histogram.bb_egress_dead);
    TEST_ASSERT_EQUAL_INT64(3, snap.histogram.bb_no_ip_watchdog);
    TEST_ASSERT_EQUAL_INT64(7, snap.histogram.inactivity);
    // Untouched buckets stay zero (present=false path exercised for these).
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.unknown);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.auth_fail);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.assoc_fail);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.handshake_timeout);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.connection_lost);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.no_ap_found);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.deauth);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.beacon_timeout);
    TEST_ASSERT_EQUAL_INT64(0, snap.histogram.assoc_leave);

    TEST_ASSERT_EQUAL_STRING("inactivity", snap.histogram.top_reason.ptr);
    TEST_ASSERT_EQUAL_INT64(7, snap.histogram.top_reason_count);

    wifi_diag_test_reset();
}

// Walks the empty-histogram snapshot: every bucket present predicate must
// evaluate false (13 present=false branches).
void test_bb_wifi_http_diag_walk_empty_histogram_omits_every_bucket(void)
{
    wifi_diag_test_reset();

    bb_wifi_http_diag_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_http_diag_fill(&snap, NULL));

    rec_reset();
    bb_serialize_walk(&bb_wifi_http_diag_desc, &snap, &s_rec_emit);

    static const char *const buckets[] = {
        "unknown", "auth_fail", "assoc_fail", "handshake_timeout",
        "connection_lost", "no_ap_found", "inactivity", "deauth",
        "beacon_timeout", "bb_lost_ip", "bb_egress_dead",
        "bb_no_ip_watchdog", "assoc_leave",
    };
    for (size_t i = 0; i < sizeof(buckets) / sizeof(buckets[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(rec_saw(buckets[i]), buckets[i]);
    }
    TEST_ASSERT_TRUE(rec_saw("top_reason"));
    TEST_ASSERT_TRUE(rec_saw("top_reason_count"));

    wifi_diag_test_reset();
}

// Walks the mixed-histogram snapshot: the 4 non-zero buckets' present
// predicates evaluate true, the other 9 evaluate false -- every present
// predicate's both branches are now exercised across these two walk tests.
void test_bb_wifi_http_diag_walk_mixed_histogram_present_gates_correct_buckets(void)
{
    wifi_diag_test_reset();

    uint16_t hist[BB_WIFI_DISC_COUNT];
    memset(hist, 0, sizeof(hist));
    hist[BB_WIFI_DISC_BB_LOST_IP]        = 1;
    hist[BB_WIFI_DISC_BB_EGRESS_DEAD]    = 4;
    hist[BB_WIFI_DISC_BB_NO_IP_WATCHDOG] = 3;
    hist[BB_WIFI_DISC_INACTIVITY]        = 7;
    bb_wifi_test_set_reason_histogram(hist, BB_WIFI_DISC_COUNT);

    bb_wifi_http_diag_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_http_diag_fill(&snap, NULL));

    rec_reset();
    bb_serialize_walk(&bb_wifi_http_diag_desc, &snap, &s_rec_emit);

    TEST_ASSERT_TRUE(rec_saw("bb_lost_ip"));
    TEST_ASSERT_TRUE(rec_saw("bb_egress_dead"));
    TEST_ASSERT_TRUE(rec_saw("bb_no_ip_watchdog"));
    TEST_ASSERT_TRUE(rec_saw("inactivity"));

    static const char *const absent_buckets[] = {
        "unknown", "auth_fail", "assoc_fail", "handshake_timeout",
        "connection_lost", "no_ap_found", "deauth", "beacon_timeout",
        "assoc_leave",
    };
    for (size_t i = 0; i < sizeof(absent_buckets) / sizeof(absent_buckets[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(rec_saw(absent_buckets[i]), absent_buckets[i]);
    }

    wifi_diag_test_reset();
}

// Registration fits the shared scratch buffer -- turns the "confirm the
// snapshot fits" requirement into an actual regression test.
void test_bb_wifi_http_diag_desc_fits_scratch(void)
{
    bb_diag_section_test_reset();

    bb_diag_section_t section = {
        .name         = "wifi",
        .desc         = "test",
        .snap_desc    = &bb_wifi_http_diag_desc,
        .fill         = bb_wifi_http_diag_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&section));

    bb_diag_section_test_reset();
}
