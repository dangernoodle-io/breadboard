// Host tests for the bb_task_registry periodic base-scan evaluator
// (task-registry unification PR3): base-registry mark/sweep reconciliation,
// low-stack transition debounce, and the overlay-join-by-handle contract
// (register()/deregister() driving bb_task's base registry).
//
// Coverage targets: bb_task_registry_base_scan_apply's every branch (null
// args, new-handle insert, already-tracked touch, low-stack transition +
// debounce + sweep, base-registry sweep/reconcile), plus
// bb_task_registry_register/deregister's base-join/unjoin side effects.

#include "unity.h"
#include "bb_task_registry.h"
#include "bb_wdt_test.h"
#include "bb_task.h"
#include "../../components/bb_task_registry/bb_task_registry_base_scan.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void local_reset(void)
{
    bb_task_registry_test_reset();
    bb_task_registry_base_scan_test_reset();
    bb_task_base_test_reset();
}

// ---------------------------------------------------------------------------
// bb_task_registry_base_scan_apply -- null/degenerate args
// ---------------------------------------------------------------------------

void test_bb_task_registry_base_scan_apply_null_rows_is_noop(void)
{
    local_reset();
    TEST_ASSERT_EQUAL_INT(0, bb_task_registry_base_scan_apply(NULL, 3, 1));
}

void test_bb_task_registry_base_scan_apply_non_positive_n_is_noop(void)
{
    local_reset();
    bb_task_registry_base_row_t rows[1] = { 0 };
    TEST_ASSERT_EQUAL_INT(0, bb_task_registry_base_scan_apply(rows, 0, 1));
}

void test_bb_task_registry_base_scan_apply_null_handle_row_skipped(void)
{
    local_reset();
    bb_task_registry_base_row_t rows[1] = { { .handle = NULL, .name = "x", .free_bytes = 100 } };
    TEST_ASSERT_EQUAL_INT(0, bb_task_registry_base_scan_apply(rows, 1, 1));
}

// ---------------------------------------------------------------------------
// New-handle insert vs already-tracked touch
// ---------------------------------------------------------------------------

typedef struct {
    void     *target;
    bool      found;
    uint32_t  stack_bytes;
    bool      wdt_arm;
} find_ctx_t;

static void find_cb(void *handle, const bb_task_base_entry_t *entry, void *ctx)
{
    find_ctx_t *fc = (find_ctx_t *)ctx;
    if (handle == fc->target) {
        fc->found       = true;
        fc->stack_bytes = entry->stack_bytes;
        fc->wdt_arm     = entry->wdt_arm;
    }
}

void test_bb_task_registry_base_scan_apply_inserts_new_handle_placeholder(void)
{
    local_reset();
    int fake;
    bb_task_registry_base_row_t rows[1] = { { .handle = &fake, .free_bytes = 1000 } };
    strncpy(rows[0].name, "new_task", sizeof(rows[0].name) - 1);

    bb_task_registry_base_scan_apply(rows, 1, 1);

    find_ctx_t fc = { .target = &fake };
    bb_task_base_foreach(find_cb, &fc);
    TEST_ASSERT_TRUE(fc.found);
    TEST_ASSERT_EQUAL_UINT32(0, fc.stack_bytes);
    TEST_ASSERT_FALSE(fc.wdt_arm);
}

void test_bb_task_registry_base_scan_apply_never_clobbers_already_tracked_handle(void)
{
    local_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "real_name", 4096, true));

    bb_task_registry_base_row_t rows[1] = { { .handle = &fake, .free_bytes = 1000 } };
    strncpy(rows[0].name, "kernel_name", sizeof(rows[0].name) - 1);

    bb_task_registry_base_scan_apply(rows, 1, 1);

    find_ctx_t fc = { .target = &fake };
    bb_task_base_foreach(find_cb, &fc);
    TEST_ASSERT_TRUE(fc.found);
    TEST_ASSERT_EQUAL_UINT32(4096, fc.stack_bytes);  // untouched
    TEST_ASSERT_TRUE(fc.wdt_arm);                     // untouched
}

// ---------------------------------------------------------------------------
// Base-registry sweep/reconcile
// ---------------------------------------------------------------------------

void test_bb_task_registry_base_scan_apply_reclaims_gone_handle_after_grace(void)
{
    local_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "gone_soon", 512, false));

    bb_task_registry_base_row_t rows[1] = { { .handle = &fake, .free_bytes = 1000 } };
    strncpy(rows[0].name, "gone_soon", sizeof(rows[0].name) - 1);

    // Scan 1: handle present -- alive.
    bb_task_registry_base_scan_apply(rows, 1, 1);
    find_ctx_t fc1 = { .target = &fake };
    bb_task_base_foreach(find_cb, &fc1);
    TEST_ASSERT_TRUE(fc1.found);

    // Scan 2: handle missing from rows -- within grace, survives.
    bb_task_registry_base_row_t empty[1] = { 0 };
    int freed = bb_task_registry_base_scan_apply(empty, 0, 2);
    TEST_ASSERT_EQUAL_INT(0, freed);

    // Scan 3: still missing -- grace exhausted, reclaimed.
    freed = bb_task_registry_base_scan_apply(empty, 0, 3);
    TEST_ASSERT_EQUAL_INT(1, freed);

    find_ctx_t fc2 = { .target = &fake };
    bb_task_base_foreach(find_cb, &fc2);
    TEST_ASSERT_FALSE(fc2.found);
}

void test_bb_task_registry_base_scan_apply_mixed_reclaim_keeps_alive_handle(void)
{
    // Two base-registered handles; only one keeps appearing in scan rows.
    // The sweep/reconcile loop must skip the still-alive handle (in_use
    // stays true) and only bb_task_base_remove() the one that actually
    // aged out -- exercises the reconcile loop's "still in_use" branch
    // alongside a genuine freed entry in the same pass.
    local_reset();
    int alive, gone;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&alive, "alive", 512, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&gone, "gone", 512, false));

    bb_task_registry_base_row_t rows[1] = { { .handle = &alive, .free_bytes = 4096 } };
    strncpy(rows[0].name, "alive", sizeof(rows[0].name) - 1);

    bb_task_registry_base_scan_apply(rows, 1, 1);  // 'gone' missed once -- within grace
    int freed = bb_task_registry_base_scan_apply(rows, 1, 2);  // 'gone' missed twice -- reclaimed
    TEST_ASSERT_EQUAL_INT(1, freed);

    find_ctx_t fc_alive = { .target = &alive };
    bb_task_base_foreach(find_cb, &fc_alive);
    TEST_ASSERT_TRUE(fc_alive.found);

    find_ctx_t fc_gone = { .target = &gone };
    bb_task_base_foreach(find_cb, &fc_gone);
    TEST_ASSERT_FALSE(fc_gone.found);
}

// ---------------------------------------------------------------------------
// Low-stack transition + debounce
// ---------------------------------------------------------------------------

typedef struct {
    int         calls;
    char        last_name[32];
    void       *last_handle;
    uint32_t    last_free_bytes;
} low_stack_capture_t;

static low_stack_capture_t s_capture;

static void low_stack_handler_cb(const char *name, void *handle, uint32_t free_bytes, void *ctx)
{
    low_stack_capture_t *cap = (low_stack_capture_t *)ctx;
    cap->calls++;
    strncpy(cap->last_name, name, sizeof(cap->last_name) - 1);
    cap->last_name[sizeof(cap->last_name) - 1] = '\0';
    cap->last_handle     = handle;
    cap->last_free_bytes = free_bytes;
}

void test_bb_task_registry_base_scan_apply_no_handler_no_low_stack_side_effects(void)
{
    local_reset();
    int fake;
    bb_task_registry_base_row_t rows[1] = { { .handle = &fake, .free_bytes = 10 } };
    strncpy(rows[0].name, "low_task", sizeof(rows[0].name) - 1);

    // No handler registered -- must not crash, must not fire anything.
    bb_task_registry_base_scan_apply(rows, 1, 1);
    TEST_ASSERT_EQUAL_INT(0, s_capture.calls);
}

void test_bb_task_registry_base_scan_apply_transition_into_low_fires_once(void)
{
    local_reset();
    memset(&s_capture, 0, sizeof(s_capture));
    bb_task_registry_set_low_stack_handler(low_stack_handler_cb, 512, &s_capture);

    int fake;
    bb_task_registry_base_row_t rows[1] = { { .handle = &fake, .free_bytes = 100 } };
    strncpy(rows[0].name, "low_task", sizeof(rows[0].name) - 1);

    bb_task_registry_base_scan_apply(rows, 1, 1);
    TEST_ASSERT_EQUAL_INT(1, s_capture.calls);
    TEST_ASSERT_EQUAL_STRING("low_task", s_capture.last_name);
    TEST_ASSERT_EQUAL_PTR(&fake, s_capture.last_handle);
    TEST_ASSERT_EQUAL_UINT32(100, s_capture.last_free_bytes);

    // Still low next scan -- debounced, no repeat fire.
    bb_task_registry_base_scan_apply(rows, 1, 2);
    TEST_ASSERT_EQUAL_INT(1, s_capture.calls);
}

void test_bb_task_registry_base_scan_apply_above_threshold_never_fires(void)
{
    local_reset();
    memset(&s_capture, 0, sizeof(s_capture));
    bb_task_registry_set_low_stack_handler(low_stack_handler_cb, 512, &s_capture);

    int fake;
    bb_task_registry_base_row_t rows[1] = { { .handle = &fake, .free_bytes = 4096 } };
    strncpy(rows[0].name, "healthy_task", sizeof(rows[0].name) - 1);

    bb_task_registry_base_scan_apply(rows, 1, 1);
    TEST_ASSERT_EQUAL_INT(0, s_capture.calls);
}

void test_bb_task_registry_base_scan_apply_recovery_then_low_again_fires_twice(void)
{
    local_reset();
    memset(&s_capture, 0, sizeof(s_capture));
    bb_task_registry_set_low_stack_handler(low_stack_handler_cb, 512, &s_capture);

    int fake;
    bb_task_registry_base_row_t rows[1] = { { .handle = &fake, .free_bytes = 100 } };
    strncpy(rows[0].name, "flappy", sizeof(rows[0].name) - 1);

    bb_task_registry_base_scan_apply(rows, 1, 1);  // low -> fires
    TEST_ASSERT_EQUAL_INT(1, s_capture.calls);

    rows[0].free_bytes = 4096;
    bb_task_registry_base_scan_apply(rows, 1, 2);  // recovers -> no fire
    TEST_ASSERT_EQUAL_INT(1, s_capture.calls);

    rows[0].free_bytes = 50;
    bb_task_registry_base_scan_apply(rows, 1, 3);  // low again -> fires
    TEST_ASSERT_EQUAL_INT(2, s_capture.calls);
}

void test_bb_task_registry_base_scan_apply_two_handles_low_independently(void)
{
    local_reset();
    memset(&s_capture, 0, sizeof(s_capture));
    bb_task_registry_set_low_stack_handler(low_stack_handler_cb, 512, &s_capture);

    int a, b;
    bb_task_registry_base_row_t rows[2] = {
        { .handle = &a, .free_bytes = 100 },
        { .handle = &b, .free_bytes = 4096 },
    };
    strncpy(rows[0].name, "low_a", sizeof(rows[0].name) - 1);
    strncpy(rows[1].name, "healthy_b", sizeof(rows[1].name) - 1);

    // Scan 1: 'a' inserted low-stack-first (exercises the low_state_mark
    // find-loop's "occupied but different handle" branch when 'b' is looked
    // up next against a table already holding 'a').
    bb_task_registry_base_scan_apply(rows, 2, 1);
    TEST_ASSERT_EQUAL_INT(1, s_capture.calls);
    TEST_ASSERT_EQUAL_STRING("low_a", s_capture.last_name);

    // Scan 2: re-mark both (same handles) -- exercises low_state_mark's
    // "found on first loop, not first slot" path for 'b'.
    rows[0].free_bytes = 4096;  // 'a' recovers
    rows[1].free_bytes = 50;    // 'b' goes low
    bb_task_registry_base_scan_apply(rows, 2, 2);
    TEST_ASSERT_EQUAL_INT(2, s_capture.calls);
    TEST_ASSERT_EQUAL_STRING("healthy_b", s_capture.last_name);
}

void test_bb_task_registry_base_scan_apply_low_state_table_full_skips_extra_handle(void)
{
    // CONFIG_BB_TASK_BASE_MAX is pinned to 8 for host tests (platformio.ini)
    // -- the low-stack debounce table shares that same capacity. Fill it
    // with 8 distinct low-stack handles in one scan, then a 9th distinct
    // low handle finds no free slot (low_state_mark returns NULL) and is
    // silently skipped -- no crash, no handler fire for the 9th.
    local_reset();
    memset(&s_capture, 0, sizeof(s_capture));
    bb_task_registry_set_low_stack_handler(low_stack_handler_cb, 512, &s_capture);

    int handles[9];
    bb_task_registry_base_row_t rows[9];
    for (int i = 0; i < 9; i++) {
        rows[i].handle     = &handles[i];
        rows[i].free_bytes = 100;
        snprintf(rows[i].name, sizeof(rows[i].name), "t%d", i);
    }

    bb_task_registry_base_scan_apply(rows, 9, 1);
    TEST_ASSERT_EQUAL_INT(8, s_capture.calls);
}

void test_bb_task_registry_base_scan_apply_low_state_swept_after_handle_gone(void)
{
    local_reset();
    memset(&s_capture, 0, sizeof(s_capture));
    bb_task_registry_set_low_stack_handler(low_stack_handler_cb, 512, &s_capture);

    int fake;
    bb_task_registry_base_row_t rows[1] = { { .handle = &fake, .free_bytes = 100 } };
    strncpy(rows[0].name, "transient", sizeof(rows[0].name) - 1);
    bb_task_registry_base_scan_apply(rows, 1, 1);  // low -> fires
    TEST_ASSERT_EQUAL_INT(1, s_capture.calls);

    // Handle gone for > grace scans -- low-state entry is swept too.
    bb_task_registry_base_row_t empty[1] = { 0 };
    bb_task_registry_base_scan_apply(empty, 0, 2);
    bb_task_registry_base_scan_apply(empty, 0, 3);

    // Same handle address reused by a brand-new low task -- must fire again
    // (not suppressed by a stale swept-but-not-cleared debounce entry).
    bb_task_registry_base_scan_apply(rows, 1, 4);
    TEST_ASSERT_EQUAL_INT(2, s_capture.calls);
}

// ---------------------------------------------------------------------------
// Overlay-join-by-handle: bb_task_registry_register/deregister drive
// bb_task's base registry (Change 2).
// ---------------------------------------------------------------------------

void test_bb_task_registry_register_joins_base_registry_by_handle(void)
{
    local_reset();
    int fake;
    bb_task_registry_token_t token;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("overlay_task", 2048, &fake, NULL, &token));

    find_ctx_t fc = { .target = &fake };
    bb_task_base_foreach(find_cb, &fc);
    TEST_ASSERT_TRUE(fc.found);
    TEST_ASSERT_EQUAL_UINT32(2048, fc.stack_bytes);
    TEST_ASSERT_FALSE(fc.wdt_arm);
}

static void count_cb(void *handle, const bb_task_base_entry_t *entry, void *ctx)
{
    (void)handle; (void)entry;
    int *count = (int *)ctx;
    (*count)++;
}

void test_bb_task_registry_register_null_handle_does_not_join_base_registry(void)
{
    local_reset();
    bb_task_registry_token_t token;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("no_handle_task", 2048, NULL, NULL, &token));

    // No handle -- nothing to join; the base registry stays empty.
    int count = 0;
    bb_task_base_foreach(count_cb, &count);
    TEST_ASSERT_EQUAL_INT(0, count);
}

void test_bb_task_registry_deregister_removes_base_registry_entry(void)
{
    local_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_register("overlay_task", 2048, &fake, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_registry_deregister(&fake));

    find_ctx_t fc = { .target = &fake };
    bb_task_base_foreach(find_cb, &fc);
    TEST_ASSERT_FALSE(fc.found);
}

void test_bb_task_registry_deregister_unregistered_handle_is_noop_for_base_registry(void)
{
    local_reset();
    int fake;
    // Never registered -- deregister returns BB_ERR_NOT_FOUND and the
    // best-effort bb_task_base_remove() call is never reached; verify no
    // crash and the base registry stays empty.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_registry_deregister(&fake));

    find_ctx_t fc = { .target = &fake };
    bb_task_base_foreach(find_cb, &fc);
    TEST_ASSERT_FALSE(fc.found);
}
