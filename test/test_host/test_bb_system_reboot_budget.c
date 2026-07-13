// Host tests for bb_system's reboot budget (B1-863): the pure decision
// core (bb_system_reboot_budget_should_allow/state_record/state_encode/
// state_decode, bb_system_elapsed_epoch_s) and the storage-backed,
// per-cause-CACHED bb_system_reboot_budget_allows_at/record_at, exercised
// with an explicit synced/now_s pair against the existing bb_storage_ram
// backend (no new fake) -- bb_system_reboot_budget.c picks backend "ram" on
// host builds (see its #ifdef ESP_PLATFORM). The per-platform
// bb_system_reboot_budget_allows/record wrapper itself is a one-line host
// stub (bb_system_host.c: `_at(cause, false, 0U)`) with nothing left to
// test beyond "it compiles and returns/no-ops" -- its real logic is exactly
// what _at already covers with explicit args.
//
// reset_all() clears the per-cause cache (bb_system_reboot_budget_
// reset_for_test) alongside storage on every call -- required because the
// cache is process-lifetime state that would otherwise leak between test
// cases for the same cause.
#include "unity.h"
#include "bb_system.h"
#include "bb_system_test.h"
#include "bb_storage.h"
#include "bb_storage_ram.h"
#include "bb_core.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/
static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_ram_test_reset();
    bb_storage_ram_register();
    // The reboot budget's per-cause cache is process-lifetime state (loaded
    // once, cached thereafter) -- reset it too, or a value loaded/recorded
    // by an earlier test leaks into this one for the same cause.
    bb_system_reboot_budget_reset_for_test();
}

static bb_system_reboot_budget_state_t s_st;

static void st_reset(void)
{
    memset(&s_st, 0, sizeof(s_st));
}

/* ---------------------------------------------------------------------------
 * bb_system_elapsed_epoch_s
 * ---------------------------------------------------------------------------*/

void test_bb_system_elapsed_epoch_s_normal(void)
{
    TEST_ASSERT_EQUAL_UINT32(500, bb_system_elapsed_epoch_s(1500, 1000));
}

void test_bb_system_elapsed_epoch_s_equal_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_system_elapsed_epoch_s(1000, 1000));
}

// Clock skew: now_s before since_s must never underflow-wrap.
void test_bb_system_elapsed_epoch_s_skew_clamps_to_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_system_elapsed_epoch_s(100, 5000));
}

/* ---------------------------------------------------------------------------
 * bb_system_reboot_budget_should_allow — cooldown boundary (at/under/over),
 * daily-cap boundary, ring-count clamp, future-timestamp skew, NULL state.
 * ---------------------------------------------------------------------------*/

void test_bb_system_reboot_budget_should_allow_never_rebooted_is_true(void)
{
    st_reset();
    TEST_ASSERT_TRUE(bb_system_reboot_budget_should_allow(1000, 1800, 4, &s_st));
}

void test_bb_system_reboot_budget_should_allow_cooldown_one_under_is_false(void)
{
    st_reset();
    s_st.last_reboot_s = 1000;
    // 1799s elapsed, min_interval 1800 -> not yet.
    TEST_ASSERT_FALSE(bb_system_reboot_budget_should_allow(2799, 1800, 4, &s_st));
}

void test_bb_system_reboot_budget_should_allow_cooldown_at_boundary_is_true(void)
{
    st_reset();
    s_st.last_reboot_s = 1000;
    // exactly 1800s elapsed -> boundary is inclusive.
    TEST_ASSERT_TRUE(bb_system_reboot_budget_should_allow(2800, 1800, 4, &s_st));
}

void test_bb_system_reboot_budget_should_allow_cooldown_one_over_is_true(void)
{
    st_reset();
    s_st.last_reboot_s = 1000;
    TEST_ASSERT_TRUE(bb_system_reboot_budget_should_allow(2801, 1800, 4, &s_st));
}

void test_bb_system_reboot_budget_should_allow_daily_cap_one_under_is_true(void)
{
    st_reset();
    uint32_t now_s = 100000;
    // 3 entries within 24h, cap is 4 -> one under the cap, still allowed.
    // Spaced so the most recent entry also clears the 1800s cooldown.
    for (int i = 2; i >= 0; i--) {
        bb_system_reboot_budget_state_record(&s_st, now_s - (uint32_t)(2000 * i) - 2000U);
    }
    TEST_ASSERT_EQUAL_UINT8(3, s_st.ring_count);
    TEST_ASSERT_TRUE(bb_system_reboot_budget_should_allow(now_s, 1800, 4, &s_st));
}

void test_bb_system_reboot_budget_should_allow_daily_cap_at_boundary_is_false(void)
{
    st_reset();
    uint32_t now_s = 100000;
    // 4 entries within 24h, cap is 4 -> exhausted (>= not >).
    for (int i = 3; i >= 0; i--) {
        bb_system_reboot_budget_state_record(&s_st, now_s - (uint32_t)(2000 * i) - 2000U);
    }
    TEST_ASSERT_EQUAL_UINT8(4, s_st.ring_count);
    TEST_ASSERT_FALSE(bb_system_reboot_budget_should_allow(now_s, 1800, 4, &s_st));
}

// A ring entry older than 24h does not count toward the daily cap.
void test_bb_system_reboot_budget_should_allow_daily_cap_excludes_stale_entries(void)
{
    st_reset();
    uint32_t now_s = 200000;
    for (int i = 0; i < 3; i++) {
        bb_system_reboot_budget_state_record(&s_st, now_s - 90000U - (uint32_t)i);
    }
    TEST_ASSERT_TRUE(bb_system_reboot_budget_should_allow(now_s, 1800, 4, &s_st));
}

// ring_count corrupted/out-of-range (> CAP_MAX) is clamped rather than
// walking past the end of reboot_s_ring[].
void test_bb_system_reboot_budget_should_allow_ring_count_over_cap_is_clamped(void)
{
    st_reset();
    for (int i = 0; i < BB_SYSTEM_REBOOT_BUDGET_CAP_MAX; i++) {
        s_st.reboot_s_ring[i] = 100000U - 1000U - (uint32_t)i; // all within 24h
    }
    s_st.ring_count = (uint8_t)(BB_SYSTEM_REBOOT_BUDGET_CAP_MAX + 3);
    TEST_ASSERT_FALSE(bb_system_reboot_budget_should_allow(100000, 1800,
                                                            BB_SYSTEM_REBOOT_BUDGET_CAP_MAX, &s_st));
}

// A ring entry timestamped after "now" (per-entry clock skew) is excluded
// from the 24h count rather than underflowing.
void test_bb_system_reboot_budget_should_allow_ring_entry_future_timestamp_excluded(void)
{
    st_reset();
    uint32_t now_s = 100000;
    s_st.reboot_s_ring[0] = now_s + 500U;
    s_st.ring_count = 1;
    s_st.ring_head  = 1;
    TEST_ASSERT_TRUE(bb_system_reboot_budget_should_allow(now_s, 1800, 1, &s_st));
}

void test_bb_system_reboot_budget_should_allow_null_state_is_false(void)
{
    TEST_ASSERT_FALSE(bb_system_reboot_budget_should_allow(100000, 1800, 4, NULL));
}

/* ---------------------------------------------------------------------------
 * bb_system_reboot_budget_state_record — ring append + wraparound.
 * ---------------------------------------------------------------------------*/

void test_bb_system_reboot_budget_state_record_appends(void)
{
    st_reset();
    bb_system_reboot_budget_state_record(&s_st, 1000);
    TEST_ASSERT_EQUAL_UINT32(1000, s_st.last_reboot_s);
    TEST_ASSERT_EQUAL_UINT8(1, s_st.ring_count);
    TEST_ASSERT_EQUAL_UINT8(1, s_st.ring_head);
    TEST_ASSERT_EQUAL_UINT32(1000, s_st.reboot_s_ring[0]);
}

void test_bb_system_reboot_budget_state_record_null_state_is_safe(void)
{
    bb_system_reboot_budget_state_record(NULL, 1000);
}

void test_bb_system_reboot_budget_state_record_wraps_at_cap(void)
{
    st_reset();
    for (uint32_t i = 0; i < (uint32_t)BB_SYSTEM_REBOOT_BUDGET_CAP_MAX + 2; i++) {
        bb_system_reboot_budget_state_record(&s_st, 1000 + i);
    }
    TEST_ASSERT_EQUAL_UINT8(BB_SYSTEM_REBOOT_BUDGET_CAP_MAX, s_st.ring_count);
    TEST_ASSERT_EQUAL_UINT8(2, s_st.ring_head);
    TEST_ASSERT_EQUAL_UINT32(1000U + BB_SYSTEM_REBOOT_BUDGET_CAP_MAX,     s_st.reboot_s_ring[0]);
    TEST_ASSERT_EQUAL_UINT32(1000U + BB_SYSTEM_REBOOT_BUDGET_CAP_MAX + 1, s_st.reboot_s_ring[1]);
    TEST_ASSERT_EQUAL_UINT32(1000U + BB_SYSTEM_REBOOT_BUDGET_CAP_MAX + 1, s_st.last_reboot_s);
}

/* ---------------------------------------------------------------------------
 * bb_system_reboot_budget_state_encode / _decode — round-trip + corrupt/
 * truncated input.
 * ---------------------------------------------------------------------------*/

void test_bb_system_reboot_budget_state_encode_decode_round_trip_zero_state(void)
{
    st_reset();
    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    TEST_ASSERT_TRUE(bb_system_reboot_budget_state_encode(&s_st, buf, sizeof(buf)));

    bb_system_reboot_budget_state_t decoded;
    memset(&decoded, 0xAA, sizeof(decoded)); // poison to prove decode fully overwrites
    TEST_ASSERT_TRUE(bb_system_reboot_budget_state_decode(buf, &decoded));
    TEST_ASSERT_EQUAL_UINT32(s_st.last_reboot_s, decoded.last_reboot_s);
    TEST_ASSERT_EQUAL_UINT8(s_st.ring_head, decoded.ring_head);
    TEST_ASSERT_EQUAL_UINT8(s_st.ring_count, decoded.ring_count);
    for (int i = 0; i < BB_SYSTEM_REBOOT_BUDGET_CAP_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT32(s_st.reboot_s_ring[i], decoded.reboot_s_ring[i]);
    }
}

void test_bb_system_reboot_budget_state_encode_decode_round_trip_partial_ring(void)
{
    st_reset();
    bb_system_reboot_budget_state_record(&s_st, 1000);
    bb_system_reboot_budget_state_record(&s_st, 2000);
    bb_system_reboot_budget_state_record(&s_st, 3000);

    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    TEST_ASSERT_TRUE(bb_system_reboot_budget_state_encode(&s_st, buf, sizeof(buf)));

    bb_system_reboot_budget_state_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    TEST_ASSERT_TRUE(bb_system_reboot_budget_state_decode(buf, &decoded));
    TEST_ASSERT_EQUAL_UINT32(3000, decoded.last_reboot_s);
    TEST_ASSERT_EQUAL_UINT8(3, decoded.ring_head);
    TEST_ASSERT_EQUAL_UINT8(3, decoded.ring_count);
    TEST_ASSERT_EQUAL_UINT32(1000, decoded.reboot_s_ring[0]);
    TEST_ASSERT_EQUAL_UINT32(2000, decoded.reboot_s_ring[1]);
    TEST_ASSERT_EQUAL_UINT32(3000, decoded.reboot_s_ring[2]);
}

void test_bb_system_reboot_budget_state_encode_decode_round_trip_ring_wrap(void)
{
    st_reset();
    for (uint32_t i = 0; i < (uint32_t)BB_SYSTEM_REBOOT_BUDGET_CAP_MAX + 3; i++) {
        bb_system_reboot_budget_state_record(&s_st, 5000 + i);
    }

    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    TEST_ASSERT_TRUE(bb_system_reboot_budget_state_encode(&s_st, buf, sizeof(buf)));

    bb_system_reboot_budget_state_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    TEST_ASSERT_TRUE(bb_system_reboot_budget_state_decode(buf, &decoded));
    TEST_ASSERT_EQUAL_UINT32(s_st.last_reboot_s, decoded.last_reboot_s);
    TEST_ASSERT_EQUAL_UINT8(s_st.ring_head, decoded.ring_head);
    TEST_ASSERT_EQUAL_UINT8(BB_SYSTEM_REBOOT_BUDGET_CAP_MAX, decoded.ring_count);
    for (int i = 0; i < BB_SYSTEM_REBOOT_BUDGET_CAP_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT32(s_st.reboot_s_ring[i], decoded.reboot_s_ring[i]);
    }
}

void test_bb_system_reboot_budget_state_encode_null_state_is_false(void)
{
    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_encode(NULL, buf, sizeof(buf)));
}

void test_bb_system_reboot_budget_state_encode_null_buf_is_false(void)
{
    st_reset();
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_encode(&s_st, NULL, 64));
}

void test_bb_system_reboot_budget_state_encode_buf_too_small_is_false(void)
{
    st_reset();
    bb_system_reboot_budget_state_record(&s_st, 1234567890U);
    char tiny[4];
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_encode(&s_st, tiny, sizeof(tiny)));
}

void test_bb_system_reboot_budget_state_encode_zero_buf_len_is_false(void)
{
    st_reset();
    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_encode(&s_st, buf, 0));
}

// Header snprintf fits, but the ring loop runs out of room partway through.
void test_bb_system_reboot_budget_state_encode_buf_too_small_mid_ring_is_false(void)
{
    st_reset();
    char buf[10];
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_encode(&s_st, buf, sizeof(buf)));
}

void test_bb_system_reboot_budget_state_decode_null_str_is_false(void)
{
    bb_system_reboot_budget_state_t out;
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_decode(NULL, &out));
}

void test_bb_system_reboot_budget_state_decode_empty_str_is_false(void)
{
    bb_system_reboot_budget_state_t out;
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_decode("", &out));
}

void test_bb_system_reboot_budget_state_decode_null_out_is_false(void)
{
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_decode("0|0|0|0,0,0,0,0,0,0,0,0,0", NULL));
}

void test_bb_system_reboot_budget_state_decode_malformed_is_false(void)
{
    bb_system_reboot_budget_state_t out;
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_decode("not a valid encoded state", &out));
}

void test_bb_system_reboot_budget_state_decode_wrong_ring_length_is_false(void)
{
    bb_system_reboot_budget_state_t out;
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_decode("0|0|0|1,2,3", &out));
}

void test_bb_system_reboot_budget_state_decode_ring_head_out_of_range_is_false(void)
{
    bb_system_reboot_budget_state_t out;
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_decode(
        "0|99|0|0,0,0,0,0,0,0,0,0,0", &out));
}

void test_bb_system_reboot_budget_state_decode_ring_count_out_of_range_is_false(void)
{
    bb_system_reboot_budget_state_t out;
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_decode(
        "0|0|99|0,0,0,0,0,0,0,0,0,0", &out));
}

void test_bb_system_reboot_budget_state_decode_malformed_ring_entry_is_false(void)
{
    bb_system_reboot_budget_state_t out;
    TEST_ASSERT_FALSE(bb_system_reboot_budget_state_decode(
        "0|0|0|1,2,x,4,5,6,7,8,9,0", &out));
}

/* ---------------------------------------------------------------------------
 * bb_system_reboot_budget_allows_at / _record_at — storage-backed
 * orchestration against the "ram" backend (bb_system_reboot_budget.c picks
 * "ram" on host builds). Covers: unsynced no-op/no-storage-I/O, persist +
 * reload round trip (cooldown), missing key, corrupt/empty/oversized
 * persisted value, out-of-range cause, and PER-CAUSE ISOLATION (the entire
 * reason per-cause state exists).
 * ---------------------------------------------------------------------------*/

// This is the test that actually PROVES the unsynced path performs no
// storage I/O. Pre-seed an EXHAUSTED budget (cooldown just recorded, daily
// cap full) directly via bb_storage_set -- if the unsynced path ever read
// storage, it would find this exhausted state and return false. Asserting
// true only demonstrates "correctly skipped storage" this way; with no
// backend registered at all (the previous version of this test), the
// assertion would pass for the WRONG reason too (a storage error falling
// through to a value that happens to be true), which cannot distinguish a
// regression that starts reading storage on the unsynced path from correct
// behavior -- see B1-863 firmware-review finding.
void test_bb_system_reboot_budget_allows_at_unsynced_is_true_no_storage_io(void)
{
    reset_all();

    uint32_t now_s = 100000;
    bb_system_reboot_budget_state_t exhausted;
    memset(&exhausted, 0, sizeof(exhausted));
    for (int i = BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP - 1; i >= 0; i--) {
        bb_system_reboot_budget_state_record(&exhausted, now_s - (uint32_t)(2000 * i) - 2000U);
    }
    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    TEST_ASSERT_TRUE(bb_system_reboot_budget_state_encode(&exhausted, buf, sizeof(buf)));

    bb_storage_addr_t addr_wifi   = { .backend = "ram", .ns_or_dir = "bb_reboot", .key = "budget_wifi" };
    bb_storage_addr_t addr_egress = { .backend = "ram", .ns_or_dir = "bb_reboot", .key = "budget_egress" };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr_wifi, buf, strlen(buf) + 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr_egress, buf, strlen(buf) + 1));

    // Sanity: the seeded state genuinely IS exhausted when read synced.
    TEST_ASSERT_FALSE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, true, now_s));
    TEST_ASSERT_FALSE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, now_s));

    // The actual assertion: unsynced must still return true -- proving the
    // exhausted persisted state was never consulted.
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, false, now_s));
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, false, now_s));
}

void test_bb_system_reboot_budget_record_at_unsynced_is_noop(void)
{
    reset_all();
    bb_system_reboot_budget_record_at(BB_REBOOT_CAUSE_EGRESS_TIER3, false, 100000);
    // Nothing persisted -- a subsequent synced allow check for the same
    // cause/time sees a fresh (never-rebooted) state.
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, 100000));
}

void test_bb_system_reboot_budget_allows_at_synced_never_rebooted_is_true(void)
{
    reset_all();
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, 100000));
}

void test_bb_system_reboot_budget_record_at_persists_across_calls(void)
{
    reset_all();
    uint32_t now_s = 100000;

    bb_system_reboot_budget_record_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, now_s);
    // Cooldown (default 1800s) not yet elapsed -> blocked.
    TEST_ASSERT_FALSE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, now_s + 100));
    // Cooldown elapsed -> allowed again.
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true,
                                                        now_s + (uint32_t)BB_SYSTEM_REBOOT_BUDGET_MIN_INTERVAL_S));
}

// PROVES the cache actually elides the second storage read: after the
// first allows_at() call (which loads+caches), mutate the underlying
// storage directly (bypassing the cache) to an EXHAUSTED state. A second
// allows_at() call for the same cause must still return the CACHED (stale,
// never-rebooted) answer -- if it instead re-read storage, it would see
// the exhausted state and return false.
void test_bb_system_reboot_budget_allows_at_caches_after_first_load(void)
{
    reset_all();
    uint32_t now_s = 100000;

    // First call: never-rebooted -> true, and this loads+caches the state.
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, now_s));

    // Mutate storage directly (NOT via record_at, which would go through
    // and update the cache too) to an exhausted state.
    bb_system_reboot_budget_state_t exhausted;
    memset(&exhausted, 0, sizeof(exhausted));
    for (int i = BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP - 1; i >= 0; i--) {
        bb_system_reboot_budget_state_record(&exhausted, now_s - (uint32_t)(2000 * i) - 2000U);
    }
    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    TEST_ASSERT_TRUE(bb_system_reboot_budget_state_encode(&exhausted, buf, sizeof(buf)));
    bb_storage_addr_t addr = { .backend = "ram", .ns_or_dir = "bb_reboot", .key = "budget_egress" };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, buf, strlen(buf) + 1));

    // Second call: must still return true (the stale cached, never-rebooted
    // state), proving the exhausted mutation in storage was never read.
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, now_s));
}

// A missing key (never recorded) reads back as a fresh state.
void test_bb_system_reboot_budget_allows_at_missing_key_is_never_rebooted(void)
{
    reset_all();
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, true, 100000));
}

// A corrupt/malformed persisted value decode-fails safely into a zero-init
// state rather than propagating the decode error or crashing.
void test_bb_system_reboot_budget_allows_at_corrupt_value_falls_back_allowed(void)
{
    reset_all();
    bb_storage_addr_t addr = { .backend = "ram", .ns_or_dir = "bb_reboot", .key = "budget_egress" };
    const char *garbage = "not a valid encoded state";
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, garbage, strlen(garbage) + 1));

    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, 100000));
}

// An empty stored value (distinct from a missing key -- bb_storage_get
// itself succeeds with BB_OK) also falls back to a fresh state.
void test_bb_system_reboot_budget_allows_at_empty_stored_value_falls_back_allowed(void)
{
    reset_all();
    bb_storage_addr_t addr = { .backend = "ram", .ns_or_dir = "bb_reboot", .key = "budget_egress" };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "", 0));

    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, 100000));
}

// A stored value LONGER than the read buffer (out_len > sizeof(buf)) --
// bb_storage_ram's partial-read semantics still return BB_OK -- also falls
// back to a fresh state rather than decoding a truncated string.
void test_bb_system_reboot_budget_allows_at_oversized_stored_value_falls_back_allowed(void)
{
    reset_all();
    bb_storage_addr_t addr = { .backend = "ram", .ns_or_dir = "bb_reboot", .key = "budget_egress" };
    char oversized[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX + 8];
    memset(oversized, '1', sizeof(oversized));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, oversized, sizeof(oversized)));

    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, 100000));
}

// An out-of-range cause has no persisted key -- treated as a fresh,
// always-allowed state rather than a crash.
void test_bb_system_reboot_budget_allows_at_invalid_cause_is_safe(void)
{
    reset_all();
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_COUNT, true, 100000));
}

void test_bb_system_reboot_budget_record_at_invalid_cause_is_safe_noop(void)
{
    reset_all();
    bb_system_reboot_budget_record_at(BB_REBOOT_CAUSE_COUNT, true, 100000);
}

// PER-CAUSE ISOLATION — the entire reason per-cause state exists: exhausting
// one cause's persisted budget must NOT block the other cause.
void test_bb_system_reboot_budget_per_cause_isolation(void)
{
    reset_all();

    uint32_t now_s = 100000;
    // Exhaust EGRESS_TIER3's daily cap (default 4) at this instant.
    for (int i = 3; i >= 0; i--) {
        bb_system_reboot_budget_record_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true,
                                           now_s - (uint32_t)(2000 * i) - 2000U);
    }

    // EGRESS_TIER3 is exhausted (daily cap reached).
    TEST_ASSERT_FALSE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_EGRESS_TIER3, true, now_s));

    // WIFI_SAFEGUARD was never touched -> its own key is independent and
    // fully allowed, proving the two causes do not share a ring.
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, true, now_s));
}

/* ---------------------------------------------------------------------------
 * bb_system_reboot_budget_allows / _record — the per-platform wrapper.
 * On host this is a straight-line call with synced=false (bb_system_host.c),
 * so it always mirrors the unsynced branch already covered above.
 * ---------------------------------------------------------------------------*/

void test_bb_system_reboot_budget_allows_is_true_on_host(void)
{
    reset_all();
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows(BB_REBOOT_CAUSE_WIFI_SAFEGUARD));
    TEST_ASSERT_TRUE(bb_system_reboot_budget_allows(BB_REBOOT_CAUSE_EGRESS_TIER3));
}

void test_bb_system_reboot_budget_record_is_noop_on_host(void)
{
    reset_all();
    bb_system_reboot_budget_record(BB_REBOOT_CAUSE_WIFI_SAFEGUARD);
    bb_system_reboot_budget_record(BB_REBOOT_CAUSE_EGRESS_TIER3);
}
