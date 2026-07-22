// Tests for bb_cache's opt-in AGE_OUT eviction policy (B1-592 A3): the LAZY
// (read-time) floor, the SWEEP backstop's single-pass logic, the register-
// time guards, bb_cache_is_stale(), and the evict-notify hook
// (bb_cache_set_evict_notify_fn(), bb_cache_internal.h) that fires on_remove
// for both eviction paths (B1-1118: bb_cache_reactive, the hook's former
// installer, is gone -- these tests install the hook directly, the same way
// a single composer does in production).
//
// Uses bb_cache_test_set_clock() (BB_CACHE_TESTING) to advance "now"
// deterministically -- no real sleeps or threads anywhere in this file.

#include "unity.h"
#include "bb_cache.h"
#include "bb_cache_internal.h"
#include "bb_json.h"

#include <string.h>
#include <stdint.h>

// Test reset/injection hooks (BB_CACHE_TESTING), defined in
// platform/espidf/bb_cache/bb_cache_espidf.c.
void bb_cache_reset_for_test(void);
void bb_cache_test_set_clock(uint64_t (*fn)(void));
void bb_cache_evict_sweep_once_for_test(void);
void bb_cache_test_set_race_hook(void (*hook)(const char *key));

// ---------------------------------------------------------------------------
// Fake clock
// ---------------------------------------------------------------------------

static uint64_t s_fake_now_ms;

static uint64_t fake_clock(void)
{
    return s_fake_now_ms;
}

// ---------------------------------------------------------------------------
// Test topic
// ---------------------------------------------------------------------------

typedef struct {
    int value;
} evict_snap_t;

static void evict_serialize(bb_json_t obj, const void *snap)
{
    const evict_snap_t *s = (const evict_snap_t *)snap;
    bb_json_obj_set_int(obj, "value", s->value);
}

static bb_err_t reg_owned_age_out(const char *key, uint32_t stale_age_ms, uint32_t evict_age_ms)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(evict_snap_t),
        .serialize = evict_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
        .eviction  = {
            .policy       = BB_CACHE_EVICT_AGE_OUT,
            .stale_age_ms = stale_age_ms,
            .evict_age_ms = evict_age_ms,
        },
    };
    return bb_cache_register(&cfg);
}

static bb_err_t reg_owned_pinned(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(evict_snap_t),
        .serialize = evict_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
        // .eviction left zero-initialized -- BB_CACHE_EVICT_PINNED.
    };
    return bb_cache_register(&cfg);
}

static bb_err_t update_value(const char *key, int value)
{
    evict_snap_t snap = { .value = value };
    bb_cache_update_t req = { .key = key, .snap = &snap, .out_changed = NULL, .ts_ms = 0 };
    return bb_cache_update(&req);
}

static evict_snap_t s_getter_snap = { .value = 9 };

static const void *evict_getter(void)
{
    return &s_getter_snap;
}

// ---------------------------------------------------------------------------
// on_remove spy
// ---------------------------------------------------------------------------

static int  s_on_remove_calls;
static char s_last_remove_key[BB_CACHE_KEY_MAX];

static void spy_on_remove(const char *key)
{
    s_on_remove_calls++;
    strncpy(s_last_remove_key, key, sizeof(s_last_remove_key) - 1);
    s_last_remove_key[sizeof(s_last_remove_key) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static void evict_setup(void)
{
    bb_cache_reset_for_test();
    bb_cache_set_evict_notify_fn(NULL);
    bb_cache_test_set_clock(fake_clock);
    s_fake_now_ms      = 1000000;  // arbitrary non-zero epoch
    s_on_remove_calls  = 0;
    s_last_remove_key[0] = '\0';
}

static void evict_teardown(void)
{
    bb_cache_set_evict_notify_fn(NULL);
    bb_cache_test_set_clock(NULL);
}

// ---------------------------------------------------------------------------
// LAZY eviction
// ---------------------------------------------------------------------------

void test_bb_cache_evict_lazy_read_before_evict_age_still_served(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.a", 42));

    s_fake_now_ms += 999;  // just under evict_age_ms

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_get_serialized("evict.a", buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(bb_cache_exists("evict.a"));
    evict_teardown();
}

void test_bb_cache_evict_lazy_read_at_evict_age_misses_and_frees(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.a", 42));

    s_fake_now_ms += 1000;  // exactly evict_age_ms

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_get_serialized("evict.a", buf, sizeof(buf), &len));
    TEST_ASSERT_FALSE(bb_cache_exists("evict.a"));
    evict_teardown();
}

void test_bb_cache_evict_lazy_fires_on_remove_via_evict_notify_hook(void)
{
    evict_setup();
    bb_cache_set_evict_notify_fn(spy_on_remove);

    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.a", 42));

    s_fake_now_ms += 1000;

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_get_serialized("evict.a", buf, sizeof(buf), &len));

    TEST_ASSERT_EQUAL(1, s_on_remove_calls);
    TEST_ASSERT_EQUAL_STRING("evict.a", s_last_remove_key);
    evict_teardown();
}

void test_bb_cache_evict_lazy_get_raw_evicts(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.a", 42));

    s_fake_now_ms += 1000;

    evict_snap_t out;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_get_raw("evict.a", &out, sizeof(out)));
    evict_teardown();
}

void test_bb_cache_evict_lazy_serialize_into_evicts(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.a", 42));

    s_fake_now_ms += 1000;

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_serialize_into("evict.a", obj));
    bb_json_free(obj);
    evict_teardown();
}

// B1-1045: bb_cache_post_serialized (bb_event-backed SSE post) is gone --
// bb_cache_get_serialized (the remaining memoized-read entry point sharing
// the LAZY eviction floor) covers this call site instead.
void test_bb_cache_evict_lazy_get_serialized_evicts(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.a", 42));

    s_fake_now_ms += 1000;

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_get_serialized("evict.a", buf, sizeof(buf), &len));
    evict_teardown();
}

void test_bb_cache_evict_pinned_policy_never_evicts(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_pinned("evict.pinned"));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.pinned", 7));

    s_fake_now_ms += 1000ULL * 1000 * 1000;  // absurdly large advance

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_get_serialized("evict.pinned", buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(bb_cache_exists("evict.pinned"));
    evict_teardown();
}

// ---------------------------------------------------------------------------
// SWEEP backstop
// ---------------------------------------------------------------------------

void test_bb_cache_evict_sweep_evicts_unread_key(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.sweep", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.sweep", 1));

    s_fake_now_ms += 1000;

    // No read of any kind between the update and the sweep -- the sweep
    // backstop must free it without a read ever touching the key.
    bb_cache_evict_sweep_once_for_test();

    TEST_ASSERT_FALSE(bb_cache_exists("evict.sweep"));
    evict_teardown();
}

void test_bb_cache_evict_sweep_skips_still_fresh_key(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.sweep", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.sweep", 1));

    s_fake_now_ms += 999;

    bb_cache_evict_sweep_once_for_test();

    TEST_ASSERT_TRUE(bb_cache_exists("evict.sweep"));
    evict_teardown();
}

void test_bb_cache_evict_sweep_skips_pinned_key(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_pinned("evict.pinned"));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.pinned", 1));

    s_fake_now_ms += 1000ULL * 1000 * 1000;

    bb_cache_evict_sweep_once_for_test();

    TEST_ASSERT_TRUE(bb_cache_exists("evict.pinned"));
    evict_teardown();
}

void test_bb_cache_evict_sweep_fires_on_remove_via_evict_notify_hook(void)
{
    evict_setup();
    bb_cache_set_evict_notify_fn(spy_on_remove);

    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.sweep", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.sweep", 1));

    s_fake_now_ms += 1000;
    bb_cache_evict_sweep_once_for_test();

    TEST_ASSERT_EQUAL(1, s_on_remove_calls);
    TEST_ASSERT_EQUAL_STRING("evict.sweep", s_last_remove_key);
    evict_teardown();
}

void test_bb_cache_evict_sweep_no_registered_keys_is_noop(void)
{
    evict_setup();
    bb_cache_evict_sweep_once_for_test();  // must not crash with an empty registry
    evict_teardown();
}

// ---------------------------------------------------------------------------
// bb_cache_is_stale
// ---------------------------------------------------------------------------

void test_bb_cache_is_stale_fresh_reports_false(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.a", 1));

    bool stale = true;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_is_stale("evict.a", &stale));
    TEST_ASSERT_FALSE(stale);
    evict_teardown();
}

void test_bb_cache_is_stale_between_stale_and_evict_reports_true(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.a", 1));

    s_fake_now_ms += 700;

    bool stale = false;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_is_stale("evict.a", &stale));
    TEST_ASSERT_TRUE(stale);
    // is_stale must NOT evict -- the key is still readable.
    TEST_ASSERT_TRUE(bb_cache_exists("evict.a"));
    evict_teardown();
}

void test_bb_cache_is_stale_pinned_policy_always_false(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_pinned("evict.pinned"));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.pinned", 1));

    s_fake_now_ms += 1000ULL * 1000 * 1000;

    bool stale = true;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_is_stale("evict.pinned", &stale));
    TEST_ASSERT_FALSE(stale);
    evict_teardown();
}

void test_bb_cache_is_stale_unknown_key_returns_not_found(void)
{
    evict_setup();
    bool stale = false;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_is_stale("evict.nope", &stale));
    evict_teardown();
}

void test_bb_cache_is_stale_null_args_return_invalid_arg(void)
{
    evict_setup();
    bool stale = false;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_is_stale(NULL, &stale));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_is_stale("evict.a", NULL));
    evict_teardown();
}

// ---------------------------------------------------------------------------
// Register-time guards (B1-592 A3, single resolved-design guard: one
// combined AGE_OUT validation with three independent rejections)
// ---------------------------------------------------------------------------

void test_bb_cache_register_age_out_zero_evict_age_rejected(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, reg_owned_age_out("evict.bad", 0, 0));
    TEST_ASSERT_FALSE(bb_cache_exists("evict.bad"));
    evict_teardown();
}

void test_bb_cache_register_age_out_stale_ge_evict_rejected(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, reg_owned_age_out("evict.bad", 1000, 1000));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, reg_owned_age_out("evict.bad", 1500, 1000));
    TEST_ASSERT_FALSE(bb_cache_exists("evict.bad"));
    evict_teardown();
}

void test_bb_cache_register_age_out_getter_mode_rejected(void)
{
    evict_setup();
    // Getter mode (snapshot != NULL) re-stamps ts_ms to now on every read, so
    // AGE_OUT is meaningless there -- rejected regardless of otherwise-valid
    // stale/evict values.
    bb_cache_config_t cfg = {
        .key       = "evict.getter",
        .snapshot  = evict_getter,
        .snap_size = 0,
        .serialize = evict_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
        .eviction  = {
            .policy       = BB_CACHE_EVICT_AGE_OUT,
            .stale_age_ms = 500,
            .evict_age_ms = 1000,
        },
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_register(&cfg));
    TEST_ASSERT_FALSE(bb_cache_exists("evict.getter"));
    evict_teardown();
}

void test_bb_cache_register_age_out_pinned_policy_ignores_stale_and_evict(void)
{
    evict_setup();
    // BB_CACHE_EVICT_PINNED is unrestricted -- non-sensical stale/evict
    // values are simply ignored (never read), no rejection.
    bb_cache_config_t cfg = {
        .key       = "evict.pinned_junk",
        .snapshot  = NULL,
        .snap_size = sizeof(evict_snap_t),
        .serialize = evict_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
        .eviction  = {
            .policy       = BB_CACHE_EVICT_PINNED,
            .stale_age_ms = 9999,
            .evict_age_ms = 0,
        },
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_TRUE(bb_cache_exists("evict.pinned_junk"));
    evict_teardown();
}

// ---------------------------------------------------------------------------
// Deterministic tombstone/generation-mismatch races (bb_cache_test_set_race_hook)
//
// Mirrors test_bb_cache_fidelity.c's race-hook idiom for bb_cache_is_stale()
// and sweep_cb() -- the two AGE_OUT-only call sites that also re-validate
// (key, generation) under e->lock but aren't covered by that file's existing
// race tests (which predate A3).
// ---------------------------------------------------------------------------

// Fires once (one-shot), between the target function's find_entry_locked_ref()
// capture and its pthread_mutex_lock(&e->lock) -- deletes and re-registers
// the same key (PINNED this time; policy is irrelevant to the mismatch
// itself) so the generation the caller captured is now stale.
static void race_delete_and_reregister(const char *key)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_delete(key));
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_pinned(key));
}

void test_bb_cache_is_stale_delete_reregister_race_returns_not_found(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.race", 500, 1000));

    bb_cache_test_set_race_hook(race_delete_and_reregister);

    bool stale = false;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_is_stale("evict.race", &stale));
    TEST_ASSERT_TRUE(bb_cache_exists("evict.race"));  // the re-register landed
    evict_teardown();
}

void test_bb_cache_evict_sweep_generation_mismatch_race_skips_key(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.sweep_race", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.sweep_race", 1));
    s_fake_now_ms += 1000;  // aged past evict_age_ms

    // Fires inside sweep_cb, between its find_entry_locked_ref() and
    // pthread_mutex_lock() -- the re-registered incarnation is PINNED, so
    // even though the key still exists after the race, it must survive this
    // sweep pass untouched (the STALE incarnation's generation is what
    // sweep_cb bails on, not a second eviction of the new one).
    bb_cache_test_set_race_hook(race_delete_and_reregister);

    bb_cache_evict_sweep_once_for_test();

    TEST_ASSERT_TRUE(bb_cache_exists("evict.sweep_race"));
    evict_teardown();
}

// Reentrant delete during a SWEEP pass: bb_cache_foreach() snapshots BOTH
// keys by value before invoking sweep_cb() for either. "evict.sweep_a"'s
// on_remove (fired from inside its own eviction) reentrantly deletes
// "evict.sweep_b" BEFORE sweep_cb() ever reaches it in the snapshot loop --
// so sweep_cb("evict.sweep_b")'s own find_entry_locked_ref() must return
// "not found" (the entry is already gone, not merely a generation mismatch).
static void reentrant_delete_sweep_b(const char *key)
{
    if (strcmp(key, "evict.sweep_a") == 0) {
        TEST_ASSERT_EQUAL(BB_OK, bb_cache_delete("evict.sweep_b"));
    }
}

void test_bb_cache_evict_sweep_reentrant_delete_key_already_gone(void)
{
    evict_setup();
    bb_cache_set_evict_notify_fn(reentrant_delete_sweep_b);

    // Registration order controls bb_cache_foreach()'s snapshot slot order --
    // "evict.sweep_a" must be registered (and therefore processed by sweep_cb)
    // BEFORE "evict.sweep_b".
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.sweep_a", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.sweep_a", 1));
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.sweep_b", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.sweep_b", 1));

    s_fake_now_ms += 1000;  // both aged past evict_age_ms

    bb_cache_evict_sweep_once_for_test();

    TEST_ASSERT_FALSE(bb_cache_exists("evict.sweep_a"));
    TEST_ASSERT_FALSE(bb_cache_exists("evict.sweep_b"));
    evict_teardown();
}

// Identity-keyed eviction guard (firmware-review fix, revises A3): the LAZY
// floor and SWEEP backstop share evict_if_aged_out_locked(), which now calls
// the internal generation-checked bb_cache_delete_if_generation() instead of
// the string-keyed public bb_cache_delete(). Its own BB_CACHE_TEST_RACE_POINT
// -- fired AFTER capturing (key, generation) and releasing e->lock, but
// BEFORE the generation-checked delete runs -- is the SECOND race point
// reached inside a single bb_cache_get_serialized() call: the FIRST fires
// earlier, inside bb_cache_get_serialized() itself (before its own e->lock is
// even taken the first time). The race hook is one-shot, so a hook installed
// directly via bb_cache_test_set_race_hook() would be consumed by that
// EARLIER point and never reach evict_if_aged_out_locked()'s own window at
// all -- and mutating identity there would just trip the pre-existing
// tombstone/generation guard in entry_matches_locked() instead (a DIFFERENT,
// already-covered branch).
//
// Two-stage chain works around the one-shot limitation: the stage-1 hook
// (installed by the test) fires at the EARLY point and does nothing but
// re-arm the REAL hook via bb_cache_test_set_race_hook() from inside its own
// body (safe -- fire_test_race_hook() nulls s_test_race_hook BEFORE invoking
// the hook, so this is not a self-clobbering re-entrant write). The re-armed
// stage-2 hook then fires at the LATE point, inside
// evict_if_aged_out_locked(), landing exactly in the window this guard
// defends.
static void race_stage2_delete_and_reregister_pinned_new_value(const char *key)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_delete(key));
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_pinned(key));
    TEST_ASSERT_EQUAL(BB_OK, update_value(key, 99));
}

static void race_stage1_arm_stage2_reregister(const char *key)
{
    (void)key;
    bb_cache_test_set_race_hook(race_stage2_delete_and_reregister_pinned_new_value);
}

void test_bb_cache_evict_lazy_generation_mismatch_race_preserves_reregistered_incarnation(void)
{
    evict_setup();
    bb_cache_set_evict_notify_fn(spy_on_remove);

    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.race_gen", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.race_gen", 42));
    s_fake_now_ms += 1000;  // aged past evict_age_ms

    bb_cache_test_set_race_hook(race_stage1_arm_stage2_reregister);

    char buf[128];
    size_t len = 0;
    // This particular read attempt still reports a miss -- the LAZY floor
    // already decided this incarnation is aged-out before the race fires, so
    // the "read misses" contract holds for the ATTEMPT -- but the generation
    // guard must have skipped the actual free/notify of the incarnation that
    // landed underneath it during the race window.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_get_serialized("evict.race_gen", buf, sizeof(buf), &len));

    TEST_ASSERT_TRUE(bb_cache_exists("evict.race_gen"));
    // on_remove fired exactly ONCE so far -- for the race hook's own explicit
    // bb_cache_delete(key) of the g0 incarnation (a real, unconditional,
    // string-keyed delete). The eviction's OWN generation-checked delete
    // attempt, running immediately after, must NOT fire a second time: its
    // captured generation (g0) no longer matches the re-registered (g1)
    // incarnation, so bb_cache_delete_if_generation() skips it silently.
    TEST_ASSERT_EQUAL(1, s_on_remove_calls);
    TEST_ASSERT_EQUAL_STRING("evict.race_gen", s_last_remove_key);

    // Confirm it's genuinely the re-registered (g1) incarnation, not a
    // half-torn-down survivor of the evicted (g0) one.
    evict_snap_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_get_raw("evict.race_gen", &out, sizeof(out)));
    TEST_ASSERT_EQUAL(99, out.value);

    // Sanity: an unrelated key with no race interference still evicts and
    // fires on_remove exactly once (bumping the count to 2) via the same
    // (matching-generation) path.
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.race_gen_control", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.race_gen_control", 7));
    s_fake_now_ms += 1000;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_get_serialized("evict.race_gen_control", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(2, s_on_remove_calls);
    TEST_ASSERT_EQUAL_STRING("evict.race_gen_control", s_last_remove_key);

    evict_teardown();
}

// Same two-stage chain (see race_stage1_arm_stage2_reregister's comment
// above), but the key is fully GONE (deleted, not re-registered) by the time
// bb_cache_delete_if_generation() runs -- covers its find_entry()-returns-NULL
// branch (BB_ERR_NOT_FOUND, silent no-op, return value ignored by
// evict_if_aged_out_locked()).
static void race_stage2_delete_only(const char *key)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_delete(key));
}

static void race_stage1_arm_stage2_delete_only(const char *key)
{
    (void)key;
    bb_cache_test_set_race_hook(race_stage2_delete_only);
}

void test_bb_cache_evict_lazy_key_deleted_during_race_is_silent_noop(void)
{
    evict_setup();
    bb_cache_set_evict_notify_fn(spy_on_remove);

    TEST_ASSERT_EQUAL(BB_OK, reg_owned_age_out("evict.race_gone", 500, 1000));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.race_gone", 1));
    s_fake_now_ms += 1000;  // aged past evict_age_ms

    bb_cache_test_set_race_hook(race_stage1_arm_stage2_delete_only);

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_get_serialized("evict.race_gone", buf, sizeof(buf), &len));

    TEST_ASSERT_FALSE(bb_cache_exists("evict.race_gone"));
    // on_remove fired exactly ONCE -- for the race hook's own explicit
    // delete. bb_cache_delete_if_generation()'s own find_entry() then finds
    // nothing at all and returns BB_ERR_NOT_FOUND without a second (bogus)
    // notify.
    TEST_ASSERT_EQUAL(1, s_on_remove_calls);
    TEST_ASSERT_EQUAL_STRING("evict.race_gone", s_last_remove_key);

    evict_teardown();
}

// ---------------------------------------------------------------------------
// NULL cfg->serialize (B1-1053 PR1 relaxation) -- a key rendered exclusively
// via bb_data has no legacy bb_json serializer; bb_cache_register() now
// accepts serialize == NULL, and the two read paths that would otherwise
// invoke a non-existent fn (bb_cache_serialize_into(), bb_cache_get_serialized())
// return BB_ERR_UNSUPPORTED instead of crashing or silently returning
// nothing.
// ---------------------------------------------------------------------------

static bb_err_t reg_owned_no_serialize(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(evict_snap_t),
        .serialize = NULL,
        .flags     = BB_CACHE_FLAG_NONE,
        // .eviction left zero-initialized -- BB_CACHE_EVICT_PINNED.
    };
    return bb_cache_register(&cfg);
}

void test_bb_cache_register_null_serialize_accepted(void)
{
    evict_setup();

    TEST_ASSERT_EQUAL(BB_OK, reg_owned_no_serialize("evict.no_serialize"));
    TEST_ASSERT_TRUE(bb_cache_exists("evict.no_serialize"));

    evict_teardown();
}

void test_bb_cache_serialize_into_null_serialize_returns_unsupported(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_no_serialize("evict.no_serialize"));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.no_serialize", 1));

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_cache_serialize_into("evict.no_serialize", obj));
    bb_json_free(obj);

    evict_teardown();
}

void test_bb_cache_get_serialized_null_serialize_returns_unsupported(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_no_serialize("evict.no_serialize"));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.no_serialize", 1));

    char   buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED,
                       bb_cache_get_serialized("evict.no_serialize", buf, sizeof(buf), &len));

    evict_teardown();
}

// A NULL-serialize key is still a fully functional bb_data-backed snapshot
// store -- bb_cache_get_raw()/bb_cache_snapshot() (the paths a bb_data
// gather hook actually uses) never touch cfg->serialize at all, so they must
// keep working unaffected by this relaxation.
void test_bb_cache_get_raw_null_serialize_still_works(void)
{
    evict_setup();
    TEST_ASSERT_EQUAL(BB_OK, reg_owned_no_serialize("evict.no_serialize"));
    TEST_ASSERT_EQUAL(BB_OK, update_value("evict.no_serialize", 7));

    evict_snap_t out = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_get_raw("evict.no_serialize", &out, sizeof(out)));
    TEST_ASSERT_EQUAL(7, out.value);

    evict_teardown();
}
