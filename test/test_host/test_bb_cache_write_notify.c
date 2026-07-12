// Tests for bb_cache's write-notify seam (B1-767 PR-3): a composed,
// outside-locks hook fired on every successful owned bb_cache_update()
// write -- mirrors the existing evict-notify hook's contract.

#include "unity.h"
#include "bb_cache.h"
#include "bb_cache_internal.h"
#include "bb_json.h"

#include <string.h>

// bb_cache_reset_for_test: defined in platform/espidf/bb_cache/bb_cache_espidf.c
void bb_cache_reset_for_test(void);

typedef struct {
    int value;
} wn_snap_t;

static void wn_serialize(bb_json_t obj, const void *snap)
{
    const wn_snap_t *s = (const wn_snap_t *)snap;
    bb_json_obj_set_int(obj, "value", s->value);
}

static bb_err_t wn_reg_owned(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(wn_snap_t),
        .serialize = wn_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

static void wn_reset(void)
{
    bb_cache_reset_for_test();
}

// ---------------------------------------------------------------------------
// Spy state
// ---------------------------------------------------------------------------

static int      s_fire_count;
static char     s_last_key[64];
static uint32_t s_last_version;

static void spy_write_notify(const char *key, uint32_t version)
{
    s_fire_count++;
    strncpy(s_last_key, key, sizeof(s_last_key) - 1);
    s_last_key[sizeof(s_last_key) - 1] = '\0';
    s_last_version = version;
}

static void spy_reset(void)
{
    s_fire_count   = 0;
    s_last_key[0]  = '\0';
    s_last_version = 0;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_cache_write_notify_default_empty_is_safe_noop(void)
{
    wn_reset();
    spy_reset();
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.default"));

    wn_snap_t s = { .value = 1 };
    // No hook installed -- must not crash, and reports nothing.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.default", .snap = &s }));
    TEST_ASSERT_EQUAL(0, s_fire_count);
}

void test_bb_cache_write_notify_fires_once_per_successful_write_with_correct_args(void)
{
    wn_reset();
    spy_reset();
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.fire"));
    bb_cache_set_write_notify_fn(spy_write_notify);

    wn_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.fire", .snap = &s }));
    TEST_ASSERT_EQUAL(1, s_fire_count);
    TEST_ASSERT_EQUAL_STRING("wn.fire", s_last_key);
    TEST_ASSERT_EQUAL_UINT32(1, s_last_version);

    s.value = 2;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.fire", .snap = &s }));
    TEST_ASSERT_EQUAL(2, s_fire_count);
    TEST_ASSERT_EQUAL_UINT32(2, s_last_version);

    bb_cache_set_write_notify_fn(NULL);
}

// A getter-mode key's bb_cache_update() is a documented no-op (no owned
// buffer to write) -- it must not fire the write-notify hook.
static wn_snap_t s_getter_backing = { .value = 9 };
static const void *wn_getter(void) { return &s_getter_backing; }

void test_bb_cache_write_notify_does_not_fire_on_getter_mode_noop_update(void)
{
    wn_reset();
    spy_reset();
    bb_cache_config_t cfg = {
        .key       = "wn.getter",
        .snapshot  = wn_getter,
        .snap_size = 0,
        .serialize = wn_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));
    bb_cache_set_write_notify_fn(spy_write_notify);

    wn_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.getter", .snap = &s }));
    TEST_ASSERT_EQUAL(0, s_fire_count);

    bb_cache_set_write_notify_fn(NULL);
}

void test_bb_cache_write_notify_does_not_fire_on_unknown_key_failed_update(void)
{
    wn_reset();
    spy_reset();
    bb_cache_set_write_notify_fn(spy_write_notify);

    wn_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_update(&(bb_cache_update_t){ .key = "wn.nope", .snap = &s }));
    TEST_ASSERT_EQUAL(0, s_fire_count);

    bb_cache_set_write_notify_fn(NULL);
}

void test_bb_cache_write_notify_composed_hook_observes_multiple_keys(void)
{
    wn_reset();
    spy_reset();
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.multi.a"));
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.multi.b"));
    bb_cache_set_write_notify_fn(spy_write_notify);

    wn_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.multi.a", .snap = &s }));
    TEST_ASSERT_EQUAL_STRING("wn.multi.a", s_last_key);
    TEST_ASSERT_EQUAL_UINT32(1, s_last_version);

    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.multi.b", .snap = &s }));
    TEST_ASSERT_EQUAL_STRING("wn.multi.b", s_last_key);
    TEST_ASSERT_EQUAL_UINT32(1, s_last_version);

    TEST_ASSERT_EQUAL(2, s_fire_count);

    bb_cache_set_write_notify_fn(NULL);
}

void test_bb_cache_write_notify_null_uninstall_stops_firing(void)
{
    wn_reset();
    spy_reset();
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.uninstall"));
    bb_cache_set_write_notify_fn(spy_write_notify);

    wn_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.uninstall", .snap = &s }));
    TEST_ASSERT_EQUAL(1, s_fire_count);

    bb_cache_set_write_notify_fn(NULL);

    s.value = 2;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.uninstall", .snap = &s }));
    TEST_ASSERT_EQUAL(1, s_fire_count);  // no additional fire after uninstall
}

// ---------------------------------------------------------------------------
// Reentrancy: the fire must happen OUTSIDE all bb_cache locks -- a hook that
// re-enters bb_cache on the SAME key (snapshot/state_version) and a
// DIFFERENT key (a bounded update) must never deadlock. Locks in this test
// itself against infinite recursion in case the hook is ever fired from
// inside e->lock (a future regression this test exists to catch).
// ---------------------------------------------------------------------------

static int s_reentrant_depth;

static void reentrant_write_notify(const char *key, uint32_t version)
{
    s_fire_count++;
    strncpy(s_last_key, key, sizeof(s_last_key) - 1);
    s_last_key[sizeof(s_last_key) - 1] = '\0';
    s_last_version = version;

    if (s_reentrant_depth > 0) return;  // guard: fire the reentrant hook at most once
    s_reentrant_depth++;

    // Re-enter on the SAME key that just fired -- proves the fire is outside
    // e->lock (bb_cache_snapshot/bb_cache_state_version take that same lock).
    uint32_t v = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version(key, &v));
    wn_snap_t buf = { 0 };
    bb_cache_snapshot_t out = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_snapshot(key, &buf, sizeof(buf), &out));

    // Bounded update on a DIFFERENT key -- proves the fire is also outside
    // s_reg_lock / any registry-wide lock.
    wn_snap_t other = { .value = 100 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){
        .key = "wn.reentrant.other", .snap = &other }));

    s_reentrant_depth--;
}

void test_bb_cache_write_notify_hook_may_reenter_bb_cache_without_deadlock(void)
{
    wn_reset();
    spy_reset();
    s_reentrant_depth = 0;
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.reentrant"));
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.reentrant.other"));
    bb_cache_set_write_notify_fn(reentrant_write_notify);

    wn_snap_t s = { .value = 1 };
    // Must return without deadlocking -- proves the fire happens outside
    // e->lock and outside s_reg_lock.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.reentrant", .snap = &s }));

    // Outer fire (depth 0) + the reentrant inner update's own fire (depth 1,
    // guarded to not recurse further) == 2.
    TEST_ASSERT_EQUAL(2, s_fire_count);

    bb_cache_set_write_notify_fn(NULL);
}

void test_bb_cache_write_notify_reset_for_test_clears_hook(void)
{
    wn_reset();
    spy_reset();
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.resettest"));
    bb_cache_set_write_notify_fn(spy_write_notify);

    // reset_for_test must clear the installed hook -- a fresh register +
    // update after reset must not fire the stale hook.
    wn_reset();
    spy_reset();
    TEST_ASSERT_EQUAL(BB_OK, wn_reg_owned("wn.resettest"));

    wn_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "wn.resettest", .snap = &s }));
    TEST_ASSERT_EQUAL(0, s_fire_count);
}
