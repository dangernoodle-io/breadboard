// Tests for bb_cache_reactive: change-driven observer layer on bb_cache
// (B1-589, PR-4b). Coverage target: 100% branch on components/.

#include "unity.h"
#include "bb_cache.h"
#include "bb_cache_reactive.h"
#include "bb_json.h"

#include <string.h>
#include <stdio.h>

// Test reset hooks (BB_CACHE_TESTING / BB_CACHE_REACTIVE_TESTING).
void bb_cache_reset_for_test(void);

// ---------------------------------------------------------------------------
// Test topics
// ---------------------------------------------------------------------------

typedef struct {
    int value;
} rx_snap_t;

static void rx_serialize(bb_json_t obj, const void *snap)
{
    const rx_snap_t *s = (const rx_snap_t *)snap;
    bb_json_obj_set_int(obj, "value", s->value);
}

// Serializer that emits a large string field so the resulting envelope
// exceeds BB_CACHE_REACTIVE_PAYLOAD_MAX (512 in the test build) -- exercises
// fire_on_change's bb_cache_get_serialized failure (buffer too small) branch.
static void rx_oversized_serialize(bb_json_t obj, const void *snap)
{
    (void)snap;
    char big[600];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    bb_json_obj_set_string(obj, "big", big);
}

static bb_err_t reg_owned(const char *key, bb_cache_serialize_fn fn)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(rx_snap_t),
        .serialize = fn,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

// ---------------------------------------------------------------------------
// Spy state
// ---------------------------------------------------------------------------

static int  s_on_change_calls;
static char s_last_key[64];
static char s_last_json[256];
static int  s_on_register_calls;
static int  s_on_remove_calls;
static bool s_reentrant_ok;

static void spy_on_change(const char *key, const char *json, size_t len,
                           int64_t ts_ms, void *ctx)
{
    (void)ctx; (void)ts_ms;
    s_on_change_calls++;
    strncpy(s_last_key, key, sizeof(s_last_key) - 1);
    s_last_key[sizeof(s_last_key) - 1] = '\0';
    size_t n = len < sizeof(s_last_json) - 1 ? len : sizeof(s_last_json) - 1;
    memcpy(s_last_json, json, n);
    s_last_json[n] = '\0';
}

static void spy_on_register(const char *key, void *ctx)
{
    (void)key; (void)ctx;
    s_on_register_calls++;
}

static void spy_on_remove(const char *key, void *ctx)
{
    (void)key; (void)ctx;
    s_on_remove_calls++;
}

// Reentrant on_change: calls bb_cache_get_serialized (public API read) from
// inside the callback, and registers a NEW observer -- both must succeed
// without deadlock because s_obs_lock is released before this callback runs.
static void spy_on_change_reentrant(const char *key, const char *json,
                                     size_t len, int64_t ts_ms, void *ctx)
{
    (void)json; (void)len; (void)ts_ms; (void)ctx;
    char buf[256];
    size_t out_len = 0;
    bb_err_t err = bb_cache_get_serialized(key, buf, sizeof(buf), &out_len);
    bb_cache_reactive_observer_t obs = {
        .key = NULL, .on_register = NULL, .on_change = NULL,
        .on_remove = NULL, .ctx = NULL,
    };
    bb_err_t err2 = bb_cache_reactive_observe(&obs);
    s_reentrant_ok = (err == BB_OK && err2 == BB_OK);
    s_on_change_calls++;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static void rx_setup(void)
{
    bb_cache_reset_for_test();
    bb_cache_reactive_reset_for_test();
    s_on_change_calls   = 0;
    s_on_register_calls = 0;
    s_on_remove_calls   = 0;
    s_reentrant_ok       = false;
    s_last_key[0]  = '\0';
    s_last_json[0] = '\0';
}

// ---------------------------------------------------------------------------
// bb_cache_reactive_observe
// ---------------------------------------------------------------------------

void test_bb_cache_reactive_observe_null_cfg_returns_invalid_arg(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_reactive_observe(NULL));
}

void test_bb_cache_reactive_observe_overlength_key_returns_invalid_arg(void)
{
    rx_setup();
    char big_key[BB_CACHE_KEY_MAX + 8];
    memset(big_key, 'k', sizeof(big_key) - 1);
    big_key[sizeof(big_key) - 1] = '\0';
    bb_cache_reactive_observer_t obs = {
        .key = big_key, .on_change = spy_on_change,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_reactive_observe(&obs));
}

void test_bb_cache_reactive_observe_pool_full_returns_no_space(void)
{
    rx_setup();
    bb_cache_reactive_observer_t obs = {
        .key = NULL, .on_change = spy_on_change,
    };
    int cap = BB_CACHE_REACTIVE_MAX_OBSERVERS;
    for (int i = 0; i < cap; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_cache_reactive_observe(&obs));
}

// ---------------------------------------------------------------------------
// bb_cache_reactive_update -- change-gated firing
// ---------------------------------------------------------------------------

void test_bb_cache_reactive_update_fires_on_change_only_when_changed(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.a", rx_serialize));

    bb_cache_reactive_observer_t obs = {
        .key = "rx.a", .on_change = spy_on_change,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 1 };
    bb_cache_update_t req = { .key = "rx.a", .snap = &snap };

    // First write: always "changed" (first-write semantics) -> fires once.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_EQUAL(1, s_on_change_calls);
    TEST_ASSERT_EQUAL_STRING("rx.a", s_last_key);

    // Identical rewrite: unchanged -> no additional fire.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_EQUAL(1, s_on_change_calls);

    // Different value: changed -> fires again.
    snap.value = 2;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_EQUAL(2, s_on_change_calls);
}

void test_bb_cache_reactive_update_key_filter_specific_key(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.a", rx_serialize));
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.b", rx_serialize));

    bb_cache_reactive_observer_t obs = {
        .key = "rx.a", .on_change = spy_on_change,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 5 };
    bb_cache_update_t req_b = { .key = "rx.b", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req_b));
    TEST_ASSERT_EQUAL(0, s_on_change_calls); // not observed

    bb_cache_update_t req_a = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req_a));
    TEST_ASSERT_EQUAL(1, s_on_change_calls);
    TEST_ASSERT_EQUAL_STRING("rx.a", s_last_key);
}

void test_bb_cache_reactive_update_observe_all_matches_every_key(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.a", rx_serialize));
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.b", rx_serialize));

    bb_cache_reactive_observer_t obs = {
        .key = NULL, .on_change = spy_on_change, // observe-all
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 7 };
    bb_cache_update_t req_a = { .key = "rx.a", .snap = &snap };
    bb_cache_update_t req_b = { .key = "rx.b", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req_a));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req_b));
    TEST_ASSERT_EQUAL(2, s_on_change_calls);
}

void test_bb_cache_reactive_update_no_observers_is_plain_passthrough(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.a", rx_serialize));

    rx_snap_t snap = { .value = 9 };
    bool changed = false;
    bb_cache_update_t req = { .key = "rx.a", .snap = &snap, .out_changed = &changed };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL(0, s_on_change_calls);
}

void test_bb_cache_reactive_update_respects_caller_out_changed(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.a", rx_serialize));

    bb_cache_reactive_observer_t obs = { .key = "rx.a", .on_change = spy_on_change };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 1 };
    bool changed = false;
    bb_cache_update_t req = { .key = "rx.a", .snap = &snap, .out_changed = &changed };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_TRUE(changed);

    // Identical rewrite: caller's out_changed reflects unchanged too.
    changed = true; // poison to prove it gets overwritten to false
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_FALSE(changed);
}

void test_bb_cache_reactive_update_null_req_returns_invalid_arg(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_reactive_update(NULL));
}

void test_bb_cache_reactive_update_unknown_key_propagates_error_no_fire(void)
{
    rx_setup();
    bb_cache_reactive_observer_t obs = { .key = NULL, .on_change = spy_on_change };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 1 };
    bb_cache_update_t req = { .key = "rx.missing", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_reactive_update(&req));
    TEST_ASSERT_EQUAL(0, s_on_change_calls);
}

// ---------------------------------------------------------------------------
// on_register / on_remove -- reserved, never invoked in PR-4b
// ---------------------------------------------------------------------------

void test_bb_cache_reactive_on_register_and_on_remove_never_invoked(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.a", rx_serialize));

    bb_cache_reactive_observer_t obs = {
        .key = "rx.a",
        .on_register = spy_on_register,
        .on_change   = spy_on_change,
        .on_remove   = spy_on_remove,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 1 };
    bb_cache_update_t req = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));

    TEST_ASSERT_EQUAL(1, s_on_change_calls);
    TEST_ASSERT_EQUAL(0, s_on_register_calls);
    TEST_ASSERT_EQUAL(0, s_on_remove_calls);
}

void test_bb_cache_reactive_observer_without_on_change_is_skipped(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.a", rx_serialize));

    // Observer with only on_register/on_remove set (no on_change) -- must be
    // skipped by fire_on_change's match scan without crashing.
    bb_cache_reactive_observer_t obs = {
        .key = "rx.a", .on_register = spy_on_register, .on_remove = spy_on_remove,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 1 };
    bb_cache_update_t req = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_EQUAL(0, s_on_change_calls);
    TEST_ASSERT_EQUAL(0, s_on_register_calls);
    TEST_ASSERT_EQUAL(0, s_on_remove_calls);
}

// ---------------------------------------------------------------------------
// Reentrancy -- snapshot-then-notify (lock released before invoke)
// ---------------------------------------------------------------------------

void test_bb_cache_reactive_on_change_callback_reentrant_no_deadlock(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.a", rx_serialize));

    bb_cache_reactive_observer_t obs = {
        .key = "rx.a", .on_change = spy_on_change_reentrant,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 3 };
    bb_cache_update_t req = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));

    TEST_ASSERT_EQUAL(1, s_on_change_calls);
    TEST_ASSERT_TRUE(s_reentrant_ok);
}

// ---------------------------------------------------------------------------
// fire_on_change internal failure branches
// ---------------------------------------------------------------------------

void test_bb_cache_reactive_fire_on_change_oversized_envelope_no_fire(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.big", rx_oversized_serialize));

    bb_cache_reactive_observer_t obs = { .key = "rx.big", .on_change = spy_on_change };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    rx_snap_t snap = { .value = 1 };
    bb_cache_update_t req = { .key = "rx.big", .snap = &snap };
    // bb_cache_update itself still succeeds; only the observer fire is
    // dropped because bb_cache_get_serialized can't fit the envelope in
    // BB_CACHE_REACTIVE_PAYLOAD_MAX bytes.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_EQUAL(0, s_on_change_calls);
}

// bb_cache always emits a well-formed envelope (bb_json can't itself produce
// unbalanced braces), so the "envelope split failed" branch in
// fire_on_change is otherwise unreachable through legitimate use -- inject a
// fake splitter (mirrors the malloc-fail injection idiom) to exercise it.
static bool fake_split_always_fails(const char *payload, int len,
                                     const char **ts_start, size_t *ts_len,
                                     const char **data_start, size_t *data_len)
{
    (void)payload; (void)len; (void)ts_start; (void)ts_len;
    (void)data_start; (void)data_len;
    return false;
}

void test_bb_cache_reactive_fire_on_change_malformed_envelope_no_fire(void)
{
    rx_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned("rx.bad", rx_serialize));

    bb_cache_reactive_observer_t obs = { .key = "rx.bad", .on_change = spy_on_change };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_observe(&obs));

    bb_cache_reactive_set_envelope_split_for_test(fake_split_always_fails);
    rx_snap_t snap = { .value = 1 };
    bb_cache_update_t req = { .key = "rx.bad", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_EQUAL(0, s_on_change_calls);
    bb_cache_reactive_set_envelope_split_for_test(NULL);
}

// ---------------------------------------------------------------------------
// Compile-out fallback (BB_CACHE_REACTIVE_ENABLE=0 static-inline path) is
// verified by the esp32c3 smoke build (reactive Kconfig off), not a host
// test -- the host test env always compiles with BB_CACHE_REACTIVE_ENABLE=1
// to exercise the real observer logic.
