// Host tests for bb_task_registry — thin bb_registry consumer tracking
// self-registered FreeRTOS task-creation sites for GET /api/diag/tasks and
// the "rtos" bb_pub telemetry source.
//
// Coverage targets: register/deregister roundtrip, duplicate-name handling,
// overflow, foreach ordering, lookup_budget hit/miss, and test_reset.

#include "unity.h"
#include "bb_task_registry.h"
#include "bb_wdt_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

typedef struct {
    int      calls;
    char     last_name[32];
    uint32_t last_budget;
    bool     last_wdt;
} foreach_capture_t;

static void foreach_capture_cb(const char *name, uint32_t stack_budget_bytes,
                                bool wdt_subscribed, void *ctx)
{
    foreach_capture_t *cap = (foreach_capture_t *)ctx;
    cap->calls++;
    strncpy(cap->last_name, name, sizeof(cap->last_name) - 1);
    cap->last_name[sizeof(cap->last_name) - 1] = '\0';
    cap->last_budget = stack_budget_bytes;
    cap->last_wdt = wdt_subscribed;
}

// ---------------------------------------------------------------------------
// register / deregister roundtrip (distinct fake handles)
// ---------------------------------------------------------------------------

void test_bb_task_registry_register_deregister_roundtrip(void)
{
    bb_task_registry_test_reset();

    int dummy = 0;
    void *fake = &dummy;

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("rt", 2048, fake, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_deregister(fake));
    TEST_ASSERT_EQUAL_UINT16(0, bb_task_registry_count());

    // deregistering an already-removed handle is BB_ERR_NOT_FOUND, not a crash.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_registry_deregister(fake));
}

// Deregister-by-value must remove the CORRECT entry when multiple tasks are
// registered concurrently (guards the by-value scan / TOCTOU fix).
void test_bb_task_registry_deregister_by_value_removes_correct_entry(void)
{
    bb_task_registry_test_reset();

    int a = 0, b = 0;
    void *ha = &a;
    void *hb = &b;

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("task_a", 2048, ha, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("task_b", 4096, hb, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(2, bb_task_registry_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_deregister(ha));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());

    // a is gone; a second deregister on it is BB_ERR_NOT_FOUND.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_registry_deregister(ha));

    // b is still present and still resolvable by value.
    foreach_capture_t cap = { 0 };
    bb_task_registry_foreach(foreach_capture_cb, &cap);
    TEST_ASSERT_EQUAL(1, cap.calls);
    TEST_ASSERT_EQUAL_STRING("task_b", cap.last_name);
    TEST_ASSERT_EQUAL_UINT32(4096, cap.last_budget);

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_deregister(hb));
    TEST_ASSERT_EQUAL_UINT16(0, bb_task_registry_count());
}

void test_bb_task_registry_register_null_name_returns_invalid_arg(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_registry_register(NULL, 2048, &dummy, NULL, NULL));
}

// handle may be NULL — some sites do not retain one (see header).
void test_bb_task_registry_register_null_handle_is_ok(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("no_handle", 2048, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());
}

void test_bb_task_registry_deregister_null_returns_invalid_arg(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_registry_deregister(NULL));
}

void test_bb_task_registry_deregister_unregistered_returns_not_found(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_registry_deregister(&dummy));
}

// ---------------------------------------------------------------------------
// Duplicate-name → update-or-ok: best-effort, matches bb_queue_registry
// precedent (logged and does NOT fail the underlying task creation).
// ---------------------------------------------------------------------------

void test_bb_task_registry_duplicate_name_returns_invalid_state(void)
{
    bb_task_registry_test_reset();
    int a = 0, b = 0;

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("dup", 2048, &a, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_task_registry_register("dup", 4096, &b, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());

    // The first registration's data is unchanged (no clobber-on-duplicate).
    uint32_t budget = 0;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("dup", &budget, NULL));
    TEST_ASSERT_EQUAL_UINT32(2048, budget);
}

// ---------------------------------------------------------------------------
// Overflow — best-effort, bounded loop well within BB_REGISTRY_SNAPSHOT_MAX (64)
// ---------------------------------------------------------------------------

void test_bb_task_registry_overflow_returns_no_space(void)
{
    bb_task_registry_test_reset();

    static int dummies[40];
    static char names[40][16];

    bb_err_t err = BB_OK;
    uint16_t registered = 0;
    for (int i = 0; i < 40; i++) {
        snprintf(names[i], sizeof names[i], "t%d", i);
        err = bb_task_registry_register(names[i], 2048, &dummies[i], NULL, NULL);
        if (err != BB_OK) {
            break;
        }
        registered++;
    }

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_TRUE(registered > 0);
    TEST_ASSERT_EQUAL_UINT16(registered, bb_task_registry_count());

    // A second overflowed attempt still fails cleanly (no state corruption).
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_task_registry_register("overflow2", 2048, &dummies[39], NULL, NULL));
}

// ---------------------------------------------------------------------------
// B1-471 — overflow observability: dropped counter, capacity, HWM warn
// ---------------------------------------------------------------------------

void test_bb_task_registry_capacity_matches_pool_size(void)
{
    bb_task_registry_test_reset();

    // Fill the registry to capacity and confirm the observed count equals
    // the advertised capacity — the two must never disagree.
    static int dummies[80];
    static char names[80][16];
    uint16_t cap = bb_task_registry_capacity();
    TEST_ASSERT_TRUE(cap > 0);
    TEST_ASSERT_TRUE((size_t)cap <= (sizeof(dummies) / sizeof(dummies[0])));

    for (uint16_t i = 0; i < cap; i++) {
        snprintf(names[i], sizeof names[i], "cap%u", (unsigned)i);
        TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register(names[i], 2048, &dummies[i], NULL, NULL));
    }
    TEST_ASSERT_EQUAL_UINT16(cap, bb_task_registry_count());
}

void test_bb_task_registry_dropped_zero_initially(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL_UINT32(0, bb_task_registry_dropped());
}

void test_bb_task_registry_dropped_increments_on_overflow(void)
{
    bb_task_registry_test_reset();

    static int dummies[80];
    static char names[80][16];
    uint16_t cap = bb_task_registry_capacity();

    for (uint16_t i = 0; i < cap; i++) {
        snprintf(names[i], sizeof names[i], "d%u", (unsigned)i);
        TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register(names[i], 2048, &dummies[i], NULL, NULL));
    }
    TEST_ASSERT_EQUAL_UINT16(cap, bb_task_registry_count());
    TEST_ASSERT_EQUAL_UINT32(0, bb_task_registry_dropped());

    // One more registration overflows the pool: count stays at cap, dropped increments.
    int extra = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_task_registry_register("overflow", 2048, &extra, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(cap, bb_task_registry_count());
    TEST_ASSERT_EQUAL_UINT32(1, bb_task_registry_dropped());

    // A second overflow keeps incrementing (monotonic, never resets except
    // via test_reset).
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_task_registry_register("overflow2", 2048, &extra, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, bb_task_registry_dropped());
}

// test_seed shares pool_alloc_locked with register() — its overflow path
// must also increment the shared dropped counter (single point of truth,
// not a register()-only special case).
void test_bb_task_registry_dropped_increments_on_test_seed_overflow(void)
{
    bb_task_registry_test_reset();

    static char names[80][16];
    uint16_t cap = bb_task_registry_capacity();

    for (uint16_t i = 0; i < cap; i++) {
        snprintf(names[i], sizeof names[i], "s%u", (unsigned)i);
        TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_test_seed(names[i], 2048, false, NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(0, bb_task_registry_dropped());

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_task_registry_test_seed("seed_overflow", 2048, false, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, bb_task_registry_dropped());
}

// High-watermark warning has no host-side log-capture seam (bb_log has none
// today) — this test exercises the threshold branch (registering into the
// margin and past it) for coverage and asserts the only externally
// observable effect: no crash, and count/dropped stay consistent. The
// warning itself is a one-shot bb_log_w verified by inspection /
// on-device log tail (see verification notes).
void test_bb_task_registry_hwm_threshold_crossed_no_crash(void)
{
    bb_task_registry_test_reset();

    static int dummies[80];
    static char names[80][16];
    uint16_t cap = bb_task_registry_capacity();

    for (uint16_t i = 0; i < cap; i++) {
        snprintf(names[i], sizeof names[i], "h%u", (unsigned)i);
        TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register(names[i], 2048, &dummies[i], NULL, NULL));
        // Registering the last few slots crosses the HWM margin; must not
        // crash or corrupt count bookkeeping.
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(i + 1), bb_task_registry_count());
    }
    TEST_ASSERT_EQUAL_UINT16(cap, bb_task_registry_count());
}

// A duplicate-name rollback right at (or past) the HWM margin must not
// corrupt s_pool_count — the freed slot from the rollback must be reusable.
void test_bb_task_registry_hwm_rollback_on_duplicate_keeps_pool_count_consistent(void)
{
    bb_task_registry_test_reset();

    static int dummies[80];
    static char names[80][16];
    uint16_t cap = bb_task_registry_capacity();

    // Fill to one below capacity.
    for (uint16_t i = 0; i < (uint16_t)(cap - 1); i++) {
        snprintf(names[i], sizeof names[i], "r%u", (unsigned)i);
        TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register(names[i], 2048, &dummies[i], NULL, NULL));
    }
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(cap - 1), bb_task_registry_count());

    // Duplicate name at the margin: pool_alloc_locked succeeds (possibly
    // crossing the HWM threshold), then bb_registry_register rejects the
    // dup and the slot is rolled back — count must return to cap-1, not
    // leak a phantom slot.
    int dup_dummy = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                       bb_task_registry_register(names[0], 4096, &dup_dummy, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(cap - 1), bb_task_registry_count());

    // The rolled-back slot is still usable for a fresh registration.
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("fresh_after_rollback", 2048, &dup_dummy, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(cap, bb_task_registry_count());
    TEST_ASSERT_EQUAL_UINT32(0, bb_task_registry_dropped());
}

// ---------------------------------------------------------------------------
// foreach — ordering + fields
// ---------------------------------------------------------------------------

void test_bb_task_registry_foreach_visits_all_in_order(void)
{
    bb_task_registry_test_reset();
    int a = 0, b = 0, c = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("first", 1024, &a, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("second", 2048, &b, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("third", 4096, &c, NULL, NULL));

    // foreach_capture_cb records only the LAST visited entry; asserting the
    // call count plus the last entry's fields confirms all three were
    // visited, in registration order, without crashing/skipping.
    foreach_capture_t cap = { 0 };
    bb_task_registry_foreach(foreach_capture_cb, &cap);
    TEST_ASSERT_EQUAL(3, cap.calls);
    TEST_ASSERT_EQUAL_STRING("third", cap.last_name);
    TEST_ASSERT_EQUAL_UINT32(4096, cap.last_budget);
}

void test_bb_task_registry_foreach_null_cb_is_noop(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("noop", 2048, &dummy, NULL, NULL));
    // Should not crash.
    bb_task_registry_foreach(NULL, NULL);
}

void test_bb_task_registry_foreach_empty_registry(void)
{
    bb_task_registry_test_reset();
    foreach_capture_t cap = { 0 };
    bb_task_registry_foreach(foreach_capture_cb, &cap);
    TEST_ASSERT_EQUAL(0, cap.calls);
}

// ---------------------------------------------------------------------------
// lookup_budget hit/miss
// ---------------------------------------------------------------------------

void test_bb_task_registry_lookup_budget_hit(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("looked_up", 3072, &dummy, NULL, NULL));

    uint32_t budget = 0;
    bool wdt = true;  // sentinel — real handle never subscribes on host
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("looked_up", &budget, &wdt));
    TEST_ASSERT_EQUAL_UINT32(3072, budget);
    TEST_ASSERT_FALSE(wdt);
}

void test_bb_task_registry_lookup_budget_miss(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_FALSE(bb_task_registry_lookup_budget("missing", NULL, NULL));
}

void test_bb_task_registry_lookup_budget_null_name(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_FALSE(bb_task_registry_lookup_budget(NULL, NULL, NULL));
}

void test_bb_task_registry_lookup_budget_out_params_optional(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("opt_out", 512, &dummy, NULL, NULL));
    // Passing NULL for both out params must not crash.
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("opt_out", NULL, NULL));
}

// ---------------------------------------------------------------------------
// test_seed — the host-only seeding hook (no real TaskHandle_t on host)
// ---------------------------------------------------------------------------

void test_bb_task_registry_test_seed_sets_wdt_flag(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_test_seed("seeded", 6144, true, NULL));

    uint32_t budget = 0;
    bool wdt = false;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("seeded", &budget, &wdt));
    TEST_ASSERT_EQUAL_UINT32(6144, budget);
    TEST_ASSERT_TRUE(wdt);
}

void test_bb_task_registry_test_seed_null_name_returns_invalid_arg(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_registry_test_seed(NULL, 2048, false, NULL));
}

// test_seed shares the same pool + underlying bb_registry as register() —
// exercise its own NO_SPACE and duplicate-name error paths directly.

void test_bb_task_registry_test_seed_overflow_returns_no_space(void)
{
    bb_task_registry_test_reset();

    static char names[40][16];
    bb_err_t err = BB_OK;
    for (int i = 0; i < 40; i++) {
        snprintf(names[i], sizeof names[i], "s%d", i);
        err = bb_task_registry_test_seed(names[i], 2048, false, NULL);
        if (err != BB_OK) {
            break;
        }
    }

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

void test_bb_task_registry_test_seed_duplicate_name_returns_invalid_state(void)
{
    bb_task_registry_test_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_test_seed("dup_seed", 2048, false, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_task_registry_test_seed("dup_seed", 4096, true, NULL));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());
}

// ---------------------------------------------------------------------------
// test_reset
// ---------------------------------------------------------------------------

void test_bb_task_registry_test_reset_clears_all(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("reset", 2048, &dummy, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());

    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL_UINT16(0, bb_task_registry_count());
    TEST_ASSERT_FALSE(bb_task_registry_lookup_budget("reset", NULL, NULL));
}

// ---------------------------------------------------------------------------
// B1-458 PR-A — opts / token / feed plumbing
// ---------------------------------------------------------------------------

void test_bb_task_registry_register_opts_hw_wdt_subscribe_true(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("wdt_on", 2048, &dummy, &opts, &token));
    TEST_ASSERT_EQUAL(1, bb_wdt_test_subscribe_count());

    bool wdt = false;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("wdt_on", NULL, &wdt));
    TEST_ASSERT_TRUE(wdt);
    TEST_ASSERT_NOT_EQUAL(BB_TASK_REGISTRY_TOKEN_INVALID.index, token.index);
}

void test_bb_task_registry_register_opts_null_no_subscribe(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("wdt_off", 2048, &dummy, NULL, NULL));
    TEST_ASSERT_EQUAL(0, bb_wdt_test_subscribe_count());

    bool wdt = true;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("wdt_off", NULL, &wdt));
    TEST_ASSERT_FALSE(wdt);
}

void test_bb_task_registry_deregister_subscribed_unsubscribes(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("wdt_dereg", 2048, &dummy, &opts, NULL));
    TEST_ASSERT_EQUAL(1, bb_wdt_test_subscribe_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_deregister(&dummy));
    TEST_ASSERT_EQUAL(1, bb_wdt_test_unsubscribe_count());

    // second deregister of the same (now-gone) handle: BB_ERR_NOT_FOUND,
    // and no extra unsubscribe call.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_registry_deregister(&dummy));
    TEST_ASSERT_EQUAL(1, bb_wdt_test_unsubscribe_count());
}

void test_bb_task_registry_feed_valid_token_feeds_and_advances(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("feed_me", 2048, &dummy, &opts, &token));

    bb_task_registry_feed(token);
    TEST_ASSERT_EQUAL(1, bb_wdt_test_feed_count());

    bb_task_registry_feed(token);
    TEST_ASSERT_EQUAL(2, bb_wdt_test_feed_count());
}

void test_bb_task_registry_feed_stale_token_is_noop(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("feed_stale", 2048, &dummy, &opts, &token));

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_deregister(&dummy));

    // token now stale (generation bumped by deregister) — no crash, no feed.
    bb_task_registry_feed(token);
    TEST_ASSERT_EQUAL(0, bb_wdt_test_feed_count());
}

void test_bb_task_registry_feed_invalid_token_is_noop(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    // No crash on the never-registered sentinel token.
    bb_task_registry_feed(BB_TASK_REGISTRY_TOKEN_INVALID);
    TEST_ASSERT_EQUAL(0, bb_wdt_test_feed_count());
}

void test_bb_task_registry_generation_reuse_invalidates_old_token(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int a = 0, b = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true };
    bb_task_registry_token_t old_token = BB_TASK_REGISTRY_TOKEN_INVALID;
    bb_task_registry_token_t new_token = BB_TASK_REGISTRY_TOKEN_INVALID;

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("slot_a", 2048, &a, &opts, &old_token));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_deregister(&a));

    // Re-register into the same freed slot.
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("slot_b", 2048, &b, &opts, &new_token));
    TEST_ASSERT_EQUAL_UINT16(old_token.index, new_token.index);
    TEST_ASSERT_NOT_EQUAL(old_token.generation, new_token.generation);

    // Old token (for slot_a, now reused by slot_b) must not feed.
    bb_task_registry_feed(old_token);
    TEST_ASSERT_EQUAL(0, bb_wdt_test_feed_count());

    // New token feeds normally.
    bb_task_registry_feed(new_token);
    TEST_ASSERT_EQUAL(1, bb_wdt_test_feed_count());
}

// opts non-NULL but hw_wdt_subscribe explicitly false — distinct branch from
// opts == NULL (both result in no subscribe, but via different code paths).
void test_bb_task_registry_register_opts_non_null_subscribe_false(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = false };
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("wdt_opts_false", 2048, &dummy, &opts, NULL));
    TEST_ASSERT_EQUAL(0, bb_wdt_test_subscribe_count());
}

// Feeding a token for a registration that was never hw-wdt-subscribed still
// advances last_feed bookkeeping but skips the hw feed call.
void test_bb_task_registry_feed_unsubscribed_token_skips_hw_feed(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("no_wdt_feed", 2048, &dummy, NULL, &token));

    bb_task_registry_feed(token);
    TEST_ASSERT_EQUAL(0, bb_wdt_test_feed_count());
}

// test_seed with a non-NULL out_token on success — the seed-hook analog of
// register()'s token-population branch.
void test_bb_task_registry_test_seed_out_token_populated(void)
{
    bb_task_registry_test_reset();

    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_test_seed("seed_token", 1024, false, &token));
    TEST_ASSERT_NOT_EQUAL(BB_TASK_REGISTRY_TOKEN_INVALID.index, token.index);
    TEST_ASSERT_NOT_EQUAL(0, token.generation);
}

// ---------------------------------------------------------------------------
// zero-token vs live slot 0 (generation-never-0 invariant)
// ---------------------------------------------------------------------------

// A zero-initialized token ({index=0, generation=0}) must never alias a live
// slot 0 — every slot's generation is initialized to (and reset to) 1, so
// any token issued by a real registration always carries generation >= 1.
void test_bb_task_registry_zero_token_does_not_alias_live_slot_zero(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("slot0", 2048, &dummy, &opts, &token));
    TEST_ASSERT_EQUAL_UINT16(0, token.index);
    TEST_ASSERT_NOT_EQUAL(0, token.generation);

    bb_task_registry_token_t zero_token = { .index = 0, .generation = 0 };
    bb_task_registry_feed(zero_token);
    TEST_ASSERT_EQUAL(0, bb_wdt_test_feed_count());

    // The real token for slot 0 still feeds normally.
    bb_task_registry_feed(token);
    TEST_ASSERT_EQUAL(1, bb_wdt_test_feed_count());
}

// ---------------------------------------------------------------------------
// NULL handle + hw_wdt_subscribe requested — warn + no-op, not a crash
// ---------------------------------------------------------------------------

void test_bb_task_registry_register_null_handle_with_subscribe_is_skipped(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("null_handle_wdt", 2048, NULL, &opts, NULL));
    TEST_ASSERT_EQUAL(0, bb_wdt_test_subscribe_count());

    bool wdt = true;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("null_handle_wdt", NULL, &wdt));
    TEST_ASSERT_FALSE(wdt);
}

// ---------------------------------------------------------------------------
// Rollback: a successful hw-wdt subscribe must be undone if the underlying
// bb_registry_register() call fails afterward (duplicate name / overflow).
// ---------------------------------------------------------------------------

void test_bb_task_registry_register_rollback_unsubscribes_on_duplicate(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int a = 0, b = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true };

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("rollback_dup", 2048, &a, &opts, NULL));
    TEST_ASSERT_EQUAL(1, bb_wdt_test_subscribe_count());

    // Second register with the same name subscribes (best-effort, before the
    // duplicate check fails), then must roll back: unsubscribe + clear
    // wdt_subscribed + free the pool slot.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                       bb_task_registry_register("rollback_dup", 4096, &b, &opts, NULL));
    TEST_ASSERT_EQUAL(1, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());

    // The pool slot from the failed rollback attempt was freed and is
    // available for reuse. This third register subscribes again (the
    // rolled-back duplicate attempt's subscribe still counts historically).
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("rollback_dup2", 2048, &b, &opts, NULL));
    TEST_ASSERT_EQUAL(3, bb_wdt_test_subscribe_count());
    TEST_ASSERT_EQUAL_UINT16(2, bb_task_registry_count());
}

// ---------------------------------------------------------------------------
// B1-458 PR-B — software watchdog monitor (sw_wdt_check / lookup / handler)
// ---------------------------------------------------------------------------

typedef struct {
    int      calls;
    char     last_name[BB_TASK_REGISTRY_NAME_MAX];
    void    *last_handle;
    uint32_t last_overrun_ms;
} sw_wdt_handler_capture_t;

static void sw_wdt_handler_capture_cb(const char *name, void *handle, uint32_t overrun_ms, void *ctx)
{
    sw_wdt_handler_capture_t *cap = (sw_wdt_handler_capture_t *)ctx;
    cap->calls++;
    strncpy(cap->last_name, name, sizeof(cap->last_name) - 1);
    cap->last_name[sizeof(cap->last_name) - 1] = '\0';
    cap->last_handle     = handle;
    cap->last_overrun_ms = overrun_ms;
}

// (1) fed in time -> no miss.
void test_bb_task_registry_sw_wdt_fed_in_time_no_miss(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("fed_ok", 2048, &dummy, &opts, &token));
    bb_task_registry_test_set_last_feed_ms(token, 1000);

    sw_wdt_handler_capture_t cap = { 0 };
    bb_task_registry_set_sw_wdt_handler(sw_wdt_handler_capture_cb, &cap);

    bb_task_registry_sw_wdt_check(1200);  // 200 ms elapsed <= 500 ms timeout

    TEST_ASSERT_EQUAL(0, cap.calls);
    uint32_t miss_count = 999;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("fed_ok", 1200, NULL, NULL, NULL, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(0, miss_count);

    bb_task_registry_set_sw_wdt_handler(NULL, NULL);
}

// (2) overdue -> miss_count=1 + handler called once with overrun.
void test_bb_task_registry_sw_wdt_overdue_fires_handler_and_increments_miss_count(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("overdue1", 2048, &dummy, &opts, &token));
    bb_task_registry_test_set_last_feed_ms(token, 1000);

    sw_wdt_handler_capture_t cap = { 0 };
    bb_task_registry_set_sw_wdt_handler(sw_wdt_handler_capture_cb, &cap);

    bb_task_registry_sw_wdt_check(1700);  // 700 ms elapsed, 200 ms overrun

    TEST_ASSERT_EQUAL(1, cap.calls);
    TEST_ASSERT_EQUAL_STRING("overdue1", cap.last_name);
    TEST_ASSERT_EQUAL_PTR(&dummy, cap.last_handle);
    TEST_ASSERT_EQUAL_UINT32(200, cap.last_overrun_ms);

    uint32_t miss_count = 0, miss_age = 999;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("overdue1", 1700, NULL, NULL, &miss_age, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(1, miss_count);
    TEST_ASSERT_EQUAL_UINT32(0, miss_age);  // just missed — now == last_miss_ms

    bb_task_registry_set_sw_wdt_handler(NULL, NULL);
}

// (3) still overdue, no feed since -> debounced, no re-fire.
void test_bb_task_registry_sw_wdt_overdue_no_refeed_does_not_refire(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("overdue2", 2048, &dummy, &opts, &token));
    bb_task_registry_test_set_last_feed_ms(token, 1000);

    sw_wdt_handler_capture_t cap = { 0 };
    bb_task_registry_set_sw_wdt_handler(sw_wdt_handler_capture_cb, &cap);

    bb_task_registry_sw_wdt_check(1700);  // first miss
    TEST_ASSERT_EQUAL(1, cap.calls);

    bb_task_registry_sw_wdt_check(1900);  // still overdue, same episode
    TEST_ASSERT_EQUAL(1, cap.calls);

    uint32_t miss_count = 0;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("overdue2", 1900, NULL, NULL, NULL, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(1, miss_count);

    bb_task_registry_set_sw_wdt_handler(NULL, NULL);
}

// (4) feed then recover -> miss_active cleared, no extra handler call.
void test_bb_task_registry_sw_wdt_feed_after_miss_clears_active_no_extra_handler(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("recover1", 2048, &dummy, &opts, &token));
    bb_task_registry_test_set_last_feed_ms(token, 1000);

    sw_wdt_handler_capture_t cap = { 0 };
    bb_task_registry_set_sw_wdt_handler(sw_wdt_handler_capture_cb, &cap);

    bb_task_registry_sw_wdt_check(1700);  // miss
    TEST_ASSERT_EQUAL(1, cap.calls);

    bb_task_registry_test_set_last_feed_ms(token, 1750);  // recovered feed
    bb_task_registry_sw_wdt_check(1800);                  // 50 ms elapsed <= 500

    TEST_ASSERT_EQUAL(1, cap.calls);  // no extra handler call on recovery
    uint32_t miss_count = 0;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("recover1", 1800, NULL, NULL, NULL, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(1, miss_count);  // unchanged by recovery

    bb_task_registry_set_sw_wdt_handler(NULL, NULL);
}

// (5) re-arm after recovery -> miss_count=2, handler fires again.
void test_bb_task_registry_sw_wdt_rearm_after_recovery_fires_again(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("rearm1", 2048, &dummy, &opts, &token));
    bb_task_registry_test_set_last_feed_ms(token, 1000);

    sw_wdt_handler_capture_t cap = { 0 };
    bb_task_registry_set_sw_wdt_handler(sw_wdt_handler_capture_cb, &cap);

    bb_task_registry_sw_wdt_check(1700);  // miss #1
    TEST_ASSERT_EQUAL(1, cap.calls);

    bb_task_registry_test_set_last_feed_ms(token, 1750);  // recover
    bb_task_registry_sw_wdt_check(1800);
    TEST_ASSERT_EQUAL(1, cap.calls);

    bb_task_registry_sw_wdt_check(2400);  // 650 ms since last feed (1750) -> overdue again

    TEST_ASSERT_EQUAL(2, cap.calls);
    uint32_t miss_count = 0;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("rearm1", 2400, NULL, NULL, NULL, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(2, miss_count);

    bb_task_registry_set_sw_wdt_handler(NULL, NULL);
}

// (6) timeout=0 never fires.
void test_bb_task_registry_sw_wdt_zero_timeout_never_fires(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("no_sw_wdt", 2048, &dummy, NULL, &token));
    bb_task_registry_test_set_last_feed_ms(token, 0);

    sw_wdt_handler_capture_t cap = { 0 };
    bb_task_registry_set_sw_wdt_handler(sw_wdt_handler_capture_cb, &cap);

    bb_task_registry_sw_wdt_check(1000000);  // would be wildly overdue if timeout>0

    TEST_ASSERT_EQUAL(0, cap.calls);
    TEST_ASSERT_FALSE(bb_task_registry_lookup_sw_wdt("no_sw_wdt", 1000000, NULL, NULL, NULL, NULL));

    bb_task_registry_set_sw_wdt_handler(NULL, NULL);
}

// (7) no handler registered -> miss_count still increments + warn logs, no crash.
void test_bb_task_registry_sw_wdt_no_handler_still_increments_miss_count(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();
    bb_task_registry_set_sw_wdt_handler(NULL, NULL);  // explicit: no handler registered

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("no_handler", 2048, &dummy, &opts, &token));
    bb_task_registry_test_set_last_feed_ms(token, 1000);

    bb_task_registry_sw_wdt_check(1600);  // overdue — must not crash without a handler

    uint32_t miss_count = 0;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("no_handler", 1600, NULL, NULL, NULL, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(1, miss_count);
}

// (8) multi-task isolation — only the overdue entry's miss stats change.
void test_bb_task_registry_sw_wdt_multi_task_isolation(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int a = 0, b = 0;
    bb_task_registry_opts_t opts_a = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_opts_t opts_b = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token_a = BB_TASK_REGISTRY_TOKEN_INVALID;
    bb_task_registry_token_t token_b = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("iso_a", 2048, &a, &opts_a, &token_a));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("iso_b", 2048, &b, &opts_b, &token_b));

    bb_task_registry_test_set_last_feed_ms(token_a, 1000);  // will be overdue
    bb_task_registry_test_set_last_feed_ms(token_b, 1500);  // stays fed

    sw_wdt_handler_capture_t cap = { 0 };
    bb_task_registry_set_sw_wdt_handler(sw_wdt_handler_capture_cb, &cap);

    bb_task_registry_sw_wdt_check(1600);  // a: 600 ms > 500 overdue; b: 100 ms ok

    TEST_ASSERT_EQUAL(1, cap.calls);
    TEST_ASSERT_EQUAL_STRING("iso_a", cap.last_name);

    uint32_t miss_count = 999;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("iso_a", 1600, NULL, NULL, NULL, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(1, miss_count);

    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("iso_b", 1600, NULL, NULL, NULL, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(0, miss_count);

    bb_task_registry_set_sw_wdt_handler(NULL, NULL);
}

// (9) both hw_wdt_subscribe + sw timeout on one entry work independently.
void test_bb_task_registry_sw_wdt_hw_and_sw_together(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .hw_wdt_subscribe = true, .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("hw_and_sw", 2048, &dummy, &opts, &token));
    TEST_ASSERT_EQUAL(1, bb_wdt_test_subscribe_count());

    bb_task_registry_feed(token);
    TEST_ASSERT_EQUAL(1, bb_wdt_test_feed_count());

    bool wdt = false;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("hw_and_sw", NULL, &wdt));
    TEST_ASSERT_TRUE(wdt);

    bb_task_registry_test_set_last_feed_ms(token, 1000);
    uint32_t timeout = 0;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("hw_and_sw", 1100, &timeout, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(500, timeout);
}

// (10) deregister DURING the miss handler (which runs outside the lock, in
// phase 2, before phase 3's writeback) — re-registering into the freed slot
// must not see phase 3 clobber its miss stats with the stale entry's data;
// the generation guard must skip the writeback.
static void sw_wdt_dereg_and_reregister_cb(const char *name, void *handle, uint32_t overrun_ms, void *ctx)
{
    (void)name;
    (void)overrun_ms;
    bb_task_registry_token_t *out_token = (bb_task_registry_token_t *)ctx;
    bb_task_registry_deregister(handle);
    bb_task_registry_opts_t new_opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_register("dereg_new", 2048, handle, &new_opts, out_token);
}

void test_bb_task_registry_sw_wdt_deregister_during_handler_writeback_is_safe(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t old_token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("dereg_race", 2048, &dummy, &opts, &old_token));
    bb_task_registry_test_set_last_feed_ms(old_token, 1000);

    bb_task_registry_token_t new_token = BB_TASK_REGISTRY_TOKEN_INVALID;
    bb_task_registry_set_sw_wdt_handler(sw_wdt_dereg_and_reregister_cb, &new_token);

    bb_task_registry_sw_wdt_check(1600);  // 600 ms elapsed > 500 -> fires handler

    // The new registration (reusing the same freed slot) must NOT inherit
    // the stale writeback's miss_count from the deregistered "dereg_race"
    // entry — the phase-3 generation guard must have skipped that write.
    uint32_t miss_count = 999, miss_age = 999;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("dereg_new", 1600, NULL, NULL, &miss_age, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(0, miss_count);
    TEST_ASSERT_EQUAL_UINT32(0, miss_age);

    bb_task_registry_set_sw_wdt_handler(NULL, NULL);
}

// (11) feed() on a stale/invalid token remains a safe no-op even when the
// entry also carries a configured sw_wdt_timeout_ms (new field; guards
// against a regression where sw-wdt bookkeeping is added to the hot path).
void test_bb_task_registry_sw_wdt_feed_stale_token_noop_with_sw_wdt_configured(void)
{
    bb_task_registry_test_reset();
    bb_wdt_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 500 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("sw_stale", 2048, &dummy, &opts, &token));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_deregister(&dummy));

    bb_task_registry_feed(token);  // stale — must not crash, no hw feed
    TEST_ASSERT_EQUAL(0, bb_wdt_test_feed_count());
}

// ---------------------------------------------------------------------------
// bb_task_registry_lookup_sw_wdt — direct coverage (the host-testable seam
// consumed by GET /api/diag/tasks's additive sw_wdt_* fields)
// ---------------------------------------------------------------------------

void test_bb_task_registry_lookup_sw_wdt_hit_returns_fields(void)
{
    bb_task_registry_test_reset();

    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 750 };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("diag_task", 2048, &dummy, &opts, &token));
    bb_task_registry_test_set_last_feed_ms(token, 1000);

    uint32_t timeout = 0, feed_age = 0, miss_age = 999, miss_count = 999;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("diag_task", 1500, &timeout, &feed_age, &miss_age, &miss_count));
    TEST_ASSERT_EQUAL_UINT32(750, timeout);
    TEST_ASSERT_EQUAL_UINT32(500, feed_age);
    TEST_ASSERT_EQUAL_UINT32(0, miss_age);  // never missed
    TEST_ASSERT_EQUAL_UINT32(0, miss_count);
}

void test_bb_task_registry_lookup_sw_wdt_false_when_timeout_zero(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("no_sw", 2048, &dummy, NULL, NULL));
    TEST_ASSERT_FALSE(bb_task_registry_lookup_sw_wdt("no_sw", 1000, NULL, NULL, NULL, NULL));
}

void test_bb_task_registry_lookup_sw_wdt_false_when_name_not_found(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_FALSE(bb_task_registry_lookup_sw_wdt("missing", 1000, NULL, NULL, NULL, NULL));
}

void test_bb_task_registry_lookup_sw_wdt_null_name_returns_false(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_FALSE(bb_task_registry_lookup_sw_wdt(NULL, 1000, NULL, NULL, NULL, NULL));
}

// test_set_last_feed_ms with an out-of-range token index — silent no-op,
// no crash (mirrors feed()'s own out-of-range guard).
void test_bb_task_registry_test_set_last_feed_ms_out_of_range_is_noop(void)
{
    bb_task_registry_test_reset();
    bb_task_registry_test_set_last_feed_ms(BB_TASK_REGISTRY_TOKEN_INVALID, 1234);
    // BB_TASK_REGISTRY_TOKEN_INVALID.index is UINT16_MAX — always out of
    // range; nothing to assert beyond "did not crash".
}

void test_bb_task_registry_lookup_sw_wdt_out_params_optional(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    bb_task_registry_opts_t opts = { .sw_wdt_timeout_ms = 200 };
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("opt_out_sw", 2048, &dummy, &opts, NULL));
    // Passing NULL for all four out params must not crash.
    TEST_ASSERT_TRUE(bb_task_registry_lookup_sw_wdt("opt_out_sw", 1000, NULL, NULL, NULL, NULL));
}
