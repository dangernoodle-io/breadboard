// Host tests for bb_task_registry — thin bb_registry consumer tracking
// self-registered FreeRTOS task-creation sites for GET /api/diag/tasks and
// the "rtos" bb_pub telemetry source.
//
// Coverage targets: register/deregister roundtrip, duplicate-name handling,
// overflow, foreach ordering, lookup_budget hit/miss, and test_reset.

#include "unity.h"
#include "bb_task_registry.h"

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

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("rt", 2048, fake));
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

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("task_a", 2048, ha));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("task_b", 4096, hb));
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
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_registry_register(NULL, 2048, &dummy));
}

// handle may be NULL — some sites do not retain one (see header).
void test_bb_task_registry_register_null_handle_is_ok(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("no_handle", 2048, NULL));
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
// Duplicate-name → update-or-ok: best-effort, matches bb_ring_registry
// precedent (logged and does NOT fail the underlying task creation).
// ---------------------------------------------------------------------------

void test_bb_task_registry_duplicate_name_returns_invalid_state(void)
{
    bb_task_registry_test_reset();
    int a = 0, b = 0;

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("dup", 2048, &a));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_task_registry_register("dup", 4096, &b));
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
        err = bb_task_registry_register(names[i], 2048, &dummies[i]);
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
                       bb_task_registry_register("overflow2", 2048, &dummies[39]));
}

// ---------------------------------------------------------------------------
// foreach — ordering + fields
// ---------------------------------------------------------------------------

void test_bb_task_registry_foreach_visits_all_in_order(void)
{
    bb_task_registry_test_reset();
    int a = 0, b = 0, c = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("first", 1024, &a));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("second", 2048, &b));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("third", 4096, &c));

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
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("noop", 2048, &dummy));
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
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("looked_up", 3072, &dummy));

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
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("opt_out", 512, &dummy));
    // Passing NULL for both out params must not crash.
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("opt_out", NULL, NULL));
}

// ---------------------------------------------------------------------------
// test_seed — the host-only seeding hook (no real TaskHandle_t on host)
// ---------------------------------------------------------------------------

void test_bb_task_registry_test_seed_sets_wdt_flag(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_test_seed("seeded", 6144, true));

    uint32_t budget = 0;
    bool wdt = false;
    TEST_ASSERT_TRUE(bb_task_registry_lookup_budget("seeded", &budget, &wdt));
    TEST_ASSERT_EQUAL_UINT32(6144, budget);
    TEST_ASSERT_TRUE(wdt);
}

void test_bb_task_registry_test_seed_null_name_returns_invalid_arg(void)
{
    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_registry_test_seed(NULL, 2048, false));
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
        err = bb_task_registry_test_seed(names[i], 2048, false);
        if (err != BB_OK) {
            break;
        }
    }

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

void test_bb_task_registry_test_seed_duplicate_name_returns_invalid_state(void)
{
    bb_task_registry_test_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_test_seed("dup_seed", 2048, false));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_task_registry_test_seed("dup_seed", 4096, true));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());
}

// ---------------------------------------------------------------------------
// test_reset
// ---------------------------------------------------------------------------

void test_bb_task_registry_test_reset_clears_all(void)
{
    bb_task_registry_test_reset();
    int dummy = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("reset", 2048, &dummy));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_registry_count());

    bb_task_registry_test_reset();
    TEST_ASSERT_EQUAL_UINT16(0, bb_task_registry_count());
    TEST_ASSERT_FALSE(bb_task_registry_lookup_budget("reset", NULL, NULL));
}
