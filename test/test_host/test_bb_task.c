// Host tests for bb_task — the SSOT task-creation primitive (2x2 create +
// pointer-keyed base registry + pure mark-and-sweep evaluator).
//
// Coverage targets: bb_task_resolve across all 2x2 quadrants (ANY/0/1 x
// DYNAMIC/STATIC) + the unicore clamp + bytes->words conversion + every
// validation-reject branch; base upsert/remove/foreach incl. re-invoke-
// same-handle upsert; sweep_apply mark/sweep incl. the single-miss grace
// survival + free-after-two-misses boundary; bb_task_create/_deregister via
// the host stub.

#include "unity.h"
#include "bb_task.h"
#include "bb_wdt.h"
#include "bb_wdt_test.h"

#include <stdint.h>
#include <string.h>

static void noop_entry(void *arg)
{
    (void)arg;
}

// ---------------------------------------------------------------------------
// bb_task_resolve — validation-reject branches
// ---------------------------------------------------------------------------

void test_bb_task_resolve_null_cfg_returns_invalid_arg(void)
{
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_resolve(NULL, 2, 0, &out));
}

void test_bb_task_resolve_null_out_returns_invalid_arg(void)
{
    bb_task_config_t cfg = { .entry = noop_entry, .name = "t", .stack_bytes = 2048 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_resolve(&cfg, 2, 0, NULL));
}

void test_bb_task_resolve_null_entry_returns_invalid_arg(void)
{
    bb_task_config_t cfg = { .entry = NULL, .name = "t", .stack_bytes = 2048 };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_resolve(&cfg, 2, 0, &out));
}

void test_bb_task_resolve_zero_stack_bytes_returns_invalid_arg(void)
{
    bb_task_config_t cfg = { .entry = noop_entry, .name = "t", .stack_bytes = 0 };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_resolve(&cfg, 2, 0, &out));
}

void test_bb_task_resolve_static_missing_stack_buf_returns_invalid_arg(void)
{
    static uint8_t tcb[64];
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .backing = BB_TASK_BACKING_STATIC,
        .stack_buf = NULL, .tcb_buf = tcb,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_resolve(&cfg, 2, 0, &out));
}

void test_bb_task_resolve_static_missing_tcb_buf_returns_invalid_arg(void)
{
    static uint8_t stack[2048];
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .backing = BB_TASK_BACKING_STATIC,
        .stack_buf = stack, .tcb_buf = NULL,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_resolve(&cfg, 2, 0, &out));
}

// ---------------------------------------------------------------------------
// bb_task_resolve — 2x2 quadrants (core ANY/0/1 x backing DYNAMIC/STATIC)
// ---------------------------------------------------------------------------

void test_bb_task_resolve_dynamic_core_any(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 4096,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, 0, &out));
    TEST_ASSERT_EQUAL_INT(BB_TASK_CORE_ANY, out.core);
    TEST_ASSERT_EQUAL_UINT32(4096, out.stack_bytes);
    TEST_ASSERT_EQUAL(BB_TASK_BACKING_DYNAMIC, out.backing);
}

void test_bb_task_resolve_dynamic_core_0(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 0, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, 0, &out));
    TEST_ASSERT_EQUAL_INT(0, out.core);
    TEST_ASSERT_EQUAL(BB_TASK_BACKING_DYNAMIC, out.backing);
}

void test_bb_task_resolve_dynamic_core_1(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 1, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, 0, &out));
    TEST_ASSERT_EQUAL_INT(1, out.core);
    TEST_ASSERT_EQUAL(BB_TASK_BACKING_DYNAMIC, out.backing);
}

void test_bb_task_resolve_static_core_any(void)
{
    static uint8_t stack[2048];
    static uint8_t tcb[64];
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_STATIC,
        .stack_buf = stack, .tcb_buf = tcb,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, 0, &out));
    TEST_ASSERT_EQUAL_INT(BB_TASK_CORE_ANY, out.core);
    TEST_ASSERT_EQUAL(BB_TASK_BACKING_STATIC, out.backing);
}

void test_bb_task_resolve_static_core_0(void)
{
    static uint8_t stack[2048];
    static uint8_t tcb[64];
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 0, .backing = BB_TASK_BACKING_STATIC,
        .stack_buf = stack, .tcb_buf = tcb,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, 0, &out));
    TEST_ASSERT_EQUAL_INT(0, out.core);
    TEST_ASSERT_EQUAL(BB_TASK_BACKING_STATIC, out.backing);
}

void test_bb_task_resolve_static_core_1(void)
{
    static uint8_t stack[2048];
    static uint8_t tcb[64];
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 1, .backing = BB_TASK_BACKING_STATIC,
        .stack_buf = stack, .tcb_buf = tcb,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, 0, &out));
    TEST_ASSERT_EQUAL_INT(1, out.core);
    TEST_ASSERT_EQUAL(BB_TASK_BACKING_STATIC, out.backing);
}

// ---------------------------------------------------------------------------
// bb_task_resolve — unicore clamp + bytes->words conversion
// ---------------------------------------------------------------------------

void test_bb_task_resolve_clamps_core_on_unicore_target(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 1, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    // num_cores=1 -- requested core 1 is out of range, clamps to ANY.
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 1, 0, &out));
    TEST_ASSERT_EQUAL_INT(BB_TASK_CORE_ANY, out.core);
}

void test_bb_task_resolve_core_any_never_clamped(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 1, 0, &out));
    TEST_ASSERT_EQUAL_INT(BB_TASK_CORE_ANY, out.core);
}

void test_bb_task_resolve_core_in_range_not_clamped(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 1, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, 0, &out));
    TEST_ASSERT_EQUAL_INT(1, out.core);
}

// The portable resolver does NOT convert units -- it passes stack_bytes
// through unchanged. The bytes -> xTaskCreate* depth conversion
// (/ sizeof(StackType_t)) happens only in the espidf shell (untestable on
// host; it's a bare sizeof, not conditional logic).
void test_bb_task_resolve_stack_bytes_passthrough_unchanged(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 8192,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 1, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(8192, out.stack_bytes);
}

// ---------------------------------------------------------------------------
// bb_task_resolve — core-claim validation + steering (B1-1364 PR2). Asserted
// THROUGH bb_task_resolve() (not by calling bb_wdt_steer_core() directly) --
// proves the wiring, not just the pure evaluators it's built from.
//
// Purity restored (B1-1356): bb_task_resolve() no longer reads bb_wdt's
// live claimed-core state itself -- the mask is passed in directly as
// `claimed_core_mask`, so these tests construct the mask literal rather
// than calling bb_wdt_claim_core()/bb_wdt_test_reset() to mutate bb_wdt's
// global state first. This makes the tests order-independent (no shared
// global to reset between them) and proves bb_task_resolve()'s steering
// decision as a pure function of its arguments.
// ---------------------------------------------------------------------------

void test_bb_task_resolve_core_owning_with_core_any_returns_invalid_arg(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .core_owning = true,
        .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_resolve(&cfg, 2, 0, &out));
}

// Two-owner rejection (B1-1364 PR3): bb_wdt's claim bitmask has no
// per-owner refcount, so a second core_owning=true request targeting an
// already-claimed core must be rejected HERE, at resolve time -- not
// silently accepted and left to eventually un-claim the first owner's
// idle-check excuse when either task deregisters (see bb_wdt.h's
// core-claim mechanism doc).
void test_bb_task_resolve_core_owning_already_claimed_core_returns_invalid_arg(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 1, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_resolve(&cfg, 2, (1U << 1), &out));
}

// A non-owning request targeting the same core is unaffected -- rejection
// applies only to a conflicting OWNING claim, not to an explicit
// (non-owning) pin sharing a core with an owner.
void test_bb_task_resolve_non_owning_pin_on_claimed_core_still_resolves(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 1, .core_owning = false, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, (1U << 1), &out));
    TEST_ASSERT_EQUAL_INT(1, out.core);
}

// A second owning claim on the OTHER (unclaimed) core is unaffected.
void test_bb_task_resolve_core_owning_different_core_still_resolves(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 0, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, (1U << 1), &out));
    TEST_ASSERT_EQUAL_INT(0, out.core);
}

void test_bb_task_resolve_steers_around_claimed_core0(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, (1U << 0), &out));
    TEST_ASSERT_EQUAL_INT(1, out.core);
}

void test_bb_task_resolve_steers_around_claimed_core1(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, (1U << 1), &out));
    TEST_ASSERT_EQUAL_INT(0, out.core);
}

void test_bb_task_resolve_explicit_pin_not_steered_even_if_claimed(void)
{
    // An explicit pin always wins over steering, even onto a core another
    // task already claims.
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 0, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, (1U << 0), &out));
    TEST_ASSERT_EQUAL_INT(0, out.core);
}

// The next two tests assert the net OUTPUT only (out.core stays
// BB_TASK_CORE_ANY) -- that result is identical whether bb_wdt_steer_core()
// was called and returned its input unchanged, or was never called at all.
// They do NOT prove bb_task_resolve() reached the steering call for these
// mask values; that reachability is proven by the two positive-steering
// tests above (test_bb_task_resolve_steers_around_claimed_core0/1), where
// an unsteered result would fail the assertion.
void test_bb_task_resolve_both_cores_claimed_does_not_steer(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, (1U << 0) | (1U << 1), &out));
    TEST_ASSERT_EQUAL_INT(BB_TASK_CORE_ANY, out.core);
}

void test_bb_task_resolve_no_claims_does_not_steer(void)
{
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 2, 0, &out));
    TEST_ASSERT_EQUAL_INT(BB_TASK_CORE_ANY, out.core);
}

void test_bb_task_resolve_core_owning_degrades_to_noop_on_unicore(void)
{
    // core_owning + an explicit core outside num_cores is degraded to
    // BB_TASK_CORE_ANY by the existing unicore clamp, same as a plain
    // (non-owning) out-of-range pin -- no new failure mode.
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 1, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 1, 0, &out));
    TEST_ASSERT_EQUAL_INT(BB_TASK_CORE_ANY, out.core);
}

void test_bb_task_resolve_core_owning_in_range_core_unaffected_by_unicore_clamp(void)
{
    // On a unicore target, core_owning targeting the one core that DOES
    // exist (core 0) is a real (not degraded) claim.
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "t", .stack_bytes = 2048,
        .core = 0, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    bb_task_resolved_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_resolve(&cfg, 1, 0, &out));
    TEST_ASSERT_EQUAL_INT(0, out.core);
}

// ---------------------------------------------------------------------------
// bb_task_base_upsert / _remove / _foreach
// ---------------------------------------------------------------------------

void test_bb_task_base_upsert_null_handle_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_base_upsert(NULL, "t", 512, false));
}

void test_bb_task_base_upsert_null_name_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_base_upsert(&fake, NULL, 512, false));
}

void test_bb_task_base_upsert_and_remove_roundtrip(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "worker", 512, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_remove(&fake));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_base_remove(&fake));
}

void test_bb_task_base_remove_null_handle_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_base_remove(NULL));
}

void test_bb_task_base_remove_unregistered_returns_not_found(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_base_remove(&fake));
}

// ---------------------------------------------------------------------------
// bb_task_base_touch
// ---------------------------------------------------------------------------

void test_bb_task_base_touch_null_handle_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_base_touch(NULL, 5));
}

void test_bb_task_base_touch_unregistered_returns_not_found(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_base_touch(&fake, 5));
}

typedef struct {
    void     *target;
    uint32_t  seen_tick;
    bool      found;
} touch_find_ctx_t;

static void touch_find_by_handle_cb(void *handle, const bb_task_base_entry_t *entry, void *ctx)
{
    touch_find_ctx_t *scan = (touch_find_ctx_t *)ctx;
    if (handle == scan->target) {
        scan->found = true;
        scan->seen_tick = entry->seen_tick;
    }
}

void test_bb_task_base_touch_updates_seen_tick_not_other_fields(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "worker", 512, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch(&fake, 7));

    touch_find_ctx_t scan = { .target = &fake };
    bb_task_base_foreach(touch_find_by_handle_cb, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_EQUAL_UINT32(7, scan.seen_tick);
}

// ---------------------------------------------------------------------------
// bb_task_base_set_free_bytes
// ---------------------------------------------------------------------------

void test_bb_task_base_set_free_bytes_null_handle_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_base_set_free_bytes(NULL, 100));
}

void test_bb_task_base_set_free_bytes_unregistered_returns_not_found(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_base_set_free_bytes(&fake, 100));
}

typedef struct {
    void     *target;
    int       core;
    bool      core_owning;
    uint32_t  seen_tick;
    bool      found;
} core_owning_find_ctx_t;

static void core_owning_find_by_handle_cb(void *handle, const bb_task_base_entry_t *entry, void *ctx)
{
    core_owning_find_ctx_t *scan = (core_owning_find_ctx_t *)ctx;
    if (handle == scan->target) {
        scan->found       = true;
        scan->core        = entry->core;
        scan->core_owning = entry->core_owning;
        scan->seen_tick   = entry->seen_tick;
    }
}

// ---------------------------------------------------------------------------
// bb_task_base_set_core_owning (B1-1364 PR2)
// ---------------------------------------------------------------------------

void test_bb_task_base_set_core_owning_null_handle_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_base_set_core_owning(NULL, 0, true));
}

void test_bb_task_base_set_core_owning_unregistered_returns_not_found(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_base_set_core_owning(&fake, 0, true));
}

void test_bb_task_base_set_core_owning_updates_fields_not_others(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "worker", 512, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch(&fake, 7));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_set_core_owning(&fake, 1, true));

    core_owning_find_ctx_t scan = { .target = &fake };
    bb_task_base_foreach(core_owning_find_by_handle_cb, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_EQUAL_INT(1, scan.core);
    TEST_ASSERT_TRUE(scan.core_owning);
    // seen_tick (set by the touch() call above) must be untouched.
    TEST_ASSERT_EQUAL_UINT32(7, scan.seen_tick);
}

typedef struct {
    void     *target;
    uint32_t  seen_tick;
    uint32_t  free_bytes;
    bool      sampled;
    bool      found;
} free_bytes_find_ctx_t;

static void free_bytes_find_by_handle_cb(void *handle, const bb_task_base_entry_t *entry, void *ctx)
{
    free_bytes_find_ctx_t *scan = (free_bytes_find_ctx_t *)ctx;
    if (handle == scan->target) {
        scan->found      = true;
        scan->seen_tick  = entry->seen_tick;
        scan->free_bytes = entry->free_bytes;
        scan->sampled    = entry->sampled;
    }
}

// [HIGH review finding] A registered task whose base-scan hasn't landed yet
// (upserted, never bb_task_base_set_free_bytes()'d) must report sampled ==
// false -- the caller-visible signal that free_bytes==0 here is "unmeasured",
// not "genuinely out of headroom". Existing set_free_bytes tests always call
// it before gathering, so they can't catch a regression of this bit.
void test_bb_task_base_upserted_task_reports_unsampled_until_set_free_bytes(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "worker", 512, true));

    free_bytes_find_ctx_t scan = { .target = &fake };
    bb_task_base_foreach(free_bytes_find_by_handle_cb, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_FALSE(scan.sampled);
    TEST_ASSERT_EQUAL_UINT32(0, scan.free_bytes);

    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_set_free_bytes(&fake, 384));

    scan = (free_bytes_find_ctx_t){ .target = &fake };
    bb_task_base_foreach(free_bytes_find_by_handle_cb, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_TRUE(scan.sampled);
    TEST_ASSERT_EQUAL_UINT32(384, scan.free_bytes);
}

void test_bb_task_base_set_free_bytes_updates_free_bytes_not_other_fields(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "worker", 512, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch(&fake, 7));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_set_free_bytes(&fake, 384));

    free_bytes_find_ctx_t scan = { .target = &fake };
    bb_task_base_foreach(free_bytes_find_by_handle_cb, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_EQUAL_UINT32(384, scan.free_bytes);
    TEST_ASSERT_TRUE(scan.sampled);
    // seen_tick (set by the touch() call above) must be untouched by
    // set_free_bytes().
    TEST_ASSERT_EQUAL_UINT32(7, scan.seen_tick);
}

typedef struct {
    int calls;
} foreach_capture_t;

static void foreach_count_cb(void *handle, const bb_task_base_entry_t *entry, void *ctx)
{
    (void)handle;
    (void)entry;
    foreach_capture_t *cap = (foreach_capture_t *)ctx;
    cap->calls++;
}

typedef struct {
    int calls;
    char last_name[BB_TASK_NAME_MAX];
    uint32_t last_stack_bytes;
    bool last_wdt_arm;
} foreach_snapshot_t;

static void foreach_snapshot_cb(void *handle, const bb_task_base_entry_t *entry, void *ctx)
{
    (void)handle;
    foreach_snapshot_t *snap = (foreach_snapshot_t *)ctx;
    snap->calls++;
    strncpy(snap->last_name, entry->name, sizeof(snap->last_name) - 1);
    snap->last_name[sizeof(snap->last_name) - 1] = '\0';
    snap->last_stack_bytes = entry->stack_bytes;
    snap->last_wdt_arm = entry->wdt_arm;
}

// Re-invocation on the same handle (pool-recycle reuse, e.g. sse_N) must be
// an update-if-present, never a double-insert.
void test_bb_task_base_upsert_reinvoke_same_handle_updates_not_duplicates(void)
{
    bb_task_base_test_reset();
    int fake;

    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "first", 256, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "second", 512, true));

    foreach_snapshot_t snap = { 0 };
    bb_task_base_foreach(foreach_snapshot_cb, &snap);
    TEST_ASSERT_EQUAL_INT(1, snap.calls);
    TEST_ASSERT_EQUAL_STRING("second", snap.last_name);
    TEST_ASSERT_EQUAL_UINT32(512, snap.last_stack_bytes);
    TEST_ASSERT_TRUE(snap.last_wdt_arm);
}

void test_bb_task_base_foreach_visits_all(void)
{
    bb_task_base_test_reset();
    int a, b, c;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&a, "a", 1, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&b, "b", 2, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&c, "c", 3, false));

    foreach_capture_t cap = { 0 };
    bb_task_base_foreach(foreach_count_cb, &cap);
    TEST_ASSERT_EQUAL_INT(3, cap.calls);
}

void test_bb_task_base_foreach_null_cb_is_noop(void)
{
    bb_task_base_test_reset();
    int fake;
    bb_task_base_upsert(&fake, "x", 1, false);
    bb_task_base_foreach(NULL, NULL);  // must not crash
}

void test_bb_task_base_foreach_empty_registry(void)
{
    bb_task_base_test_reset();
    foreach_capture_t cap = { 0 };
    bb_task_base_foreach(foreach_count_cb, &cap);
    TEST_ASSERT_EQUAL_INT(0, cap.calls);
}

void test_bb_task_base_upsert_overflow_returns_no_space(void)
{
    bb_task_base_test_reset();
    // CONFIG_BB_TASK_BASE_MAX pinned to 8 for host tests (platformio.ini).
    int handles[9];
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&handles[i], "t", 1, false));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_task_base_upsert(&handles[8], "overflow", 1, false));
}

void test_bb_task_base_test_reset_clears_all(void)
{
    bb_task_base_test_reset();
    int fake;
    bb_task_base_upsert(&fake, "x", 1, false);
    bb_task_base_test_reset();

    foreach_capture_t cap = { 0 };
    bb_task_base_foreach(foreach_count_cb, &cap);
    TEST_ASSERT_EQUAL_INT(0, cap.calls);
    // slot is free again after reset -- re-upsert must succeed cleanly.
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "x", 1, false));
}

// ---------------------------------------------------------------------------
// bb_task_base_sweep_apply — pure mark-and-sweep, standalone array
// ---------------------------------------------------------------------------

#define TEST_SWEEP_CAP 4

static void reset_entries(bb_task_base_entry_t *entries, int cap)
{
    memset(entries, 0, sizeof(*entries) * (size_t)cap);
}

void test_bb_task_base_sweep_apply_null_entries_is_noop(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_task_base_sweep_apply(NULL, TEST_SWEEP_CAP, 1));
}

void test_bb_task_base_sweep_apply_non_positive_n_is_noop(void)
{
    bb_task_base_entry_t entries[TEST_SWEEP_CAP];
    reset_entries(entries, TEST_SWEEP_CAP);
    TEST_ASSERT_EQUAL_INT(0, bb_task_base_sweep_apply(entries, 0, 1));
}

void test_bb_task_base_sweep_apply_skips_not_in_use(void)
{
    bb_task_base_entry_t entries[TEST_SWEEP_CAP];
    reset_entries(entries, TEST_SWEEP_CAP);
    // in_use defaults to false after memset -- nothing to sweep.
    TEST_ASSERT_EQUAL_INT(0, bb_task_base_sweep_apply(entries, TEST_SWEEP_CAP, 5));
}

void test_bb_task_base_sweep_apply_seen_this_tick_survives(void)
{
    bb_task_base_entry_t entries[TEST_SWEEP_CAP];
    reset_entries(entries, TEST_SWEEP_CAP);
    entries[0].in_use = true;
    entries[0].seen_tick = 3;

    int freed = bb_task_base_sweep_apply(entries, TEST_SWEEP_CAP, 3);
    TEST_ASSERT_EQUAL_INT(0, freed);
    TEST_ASSERT_TRUE(entries[0].in_use);
}

// Grace window: a single missed scan must survive.
void test_bb_task_base_sweep_apply_single_miss_survives_grace(void)
{
    bb_task_base_entry_t entries[TEST_SWEEP_CAP];
    reset_entries(entries, TEST_SWEEP_CAP);
    entries[0].in_use = true;
    entries[0].seen_tick = 1;  // last seen at tick 1

    // Missed tick 2 -- within grace (missed by exactly 1).
    int freed = bb_task_base_sweep_apply(entries, TEST_SWEEP_CAP, 2);
    TEST_ASSERT_EQUAL_INT(0, freed);
    TEST_ASSERT_TRUE(entries[0].in_use);
}

// Boundary: missed by MORE than one consecutive scan -> freed.
void test_bb_task_base_sweep_apply_frees_after_two_misses(void)
{
    bb_task_base_entry_t entries[TEST_SWEEP_CAP];
    reset_entries(entries, TEST_SWEEP_CAP);
    entries[0].in_use = true;
    entries[0].seen_tick = 1;
    entries[0].handle = (void *)0x1234;
    strncpy(entries[0].name, "gone", sizeof(entries[0].name) - 1);

    // tick 2: within grace, survives.
    TEST_ASSERT_EQUAL_INT(0, bb_task_base_sweep_apply(entries, TEST_SWEEP_CAP, 2));
    TEST_ASSERT_TRUE(entries[0].in_use);

    // tick 3: missed by 2 consecutive scans, grace exhausted, swept.
    int freed = bb_task_base_sweep_apply(entries, TEST_SWEEP_CAP, 3);
    TEST_ASSERT_EQUAL_INT(1, freed);
    TEST_ASSERT_FALSE(entries[0].in_use);
    TEST_ASSERT_EQUAL_UINT32(0, entries[0].seen_tick);
}

void test_bb_task_base_sweep_apply_keeps_live_entry_remarked_each_tick(void)
{
    bb_task_base_entry_t entries[TEST_SWEEP_CAP];
    reset_entries(entries, TEST_SWEEP_CAP);
    entries[0].in_use = true;
    entries[0].seen_tick = 1;

    entries[0].seen_tick = 2;  // simulate re-mark (still alive) on tick 2
    int freed = bb_task_base_sweep_apply(entries, TEST_SWEEP_CAP, 2);
    TEST_ASSERT_EQUAL_INT(0, freed);
    TEST_ASSERT_TRUE(entries[0].in_use);
}

void test_bb_task_base_sweep_apply_multiple_entries_mixed(void)
{
    bb_task_base_entry_t entries[TEST_SWEEP_CAP];
    reset_entries(entries, TEST_SWEEP_CAP);

    entries[0].in_use = true;
    entries[0].seen_tick = 5;  // alive this tick

    entries[1].in_use = true;
    entries[1].seen_tick = 3;  // missed by 2 -- freed

    entries[2].in_use = false; // not in use -- skipped

    entries[3].in_use = true;
    entries[3].seen_tick = 4;  // missed by 1 -- within grace

    int freed = bb_task_base_sweep_apply(entries, TEST_SWEEP_CAP, 5);
    TEST_ASSERT_EQUAL_INT(1, freed);
    TEST_ASSERT_TRUE(entries[0].in_use);
    TEST_ASSERT_FALSE(entries[1].in_use);
    TEST_ASSERT_FALSE(entries[2].in_use);
    TEST_ASSERT_TRUE(entries[3].in_use);
}

// ---------------------------------------------------------------------------
// bb_task_base_touch_or_insert -- atomic touch-or-insert
// ---------------------------------------------------------------------------

void test_bb_task_base_touch_or_insert_null_handle_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_base_touch_or_insert(NULL, "t", 1));
}

void test_bb_task_base_touch_or_insert_null_name_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_base_touch_or_insert(&fake, NULL, 1));
}

void test_bb_task_base_touch_or_insert_new_handle_inserts_placeholder(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch_or_insert(&fake, "scanned", 5));

    touch_find_ctx_t scan = { .target = &fake };
    bb_task_base_foreach(touch_find_by_handle_cb, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_EQUAL_UINT32(5, scan.seen_tick);
}

void test_bb_task_base_touch_or_insert_existing_handle_touches_only(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "worker", 4096, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch_or_insert(&fake, "kernel_name", 9));

    foreach_snapshot_t snap = { 0 };
    bb_task_base_foreach(foreach_snapshot_cb, &snap);
    TEST_ASSERT_EQUAL_INT(1, snap.calls);
    TEST_ASSERT_EQUAL_STRING("worker", snap.last_name);      // untouched
    TEST_ASSERT_EQUAL_UINT32(4096, snap.last_stack_bytes);   // untouched
    TEST_ASSERT_TRUE(snap.last_wdt_arm);                     // untouched
}

void test_bb_task_base_touch_or_insert_overflow_returns_no_space(void)
{
    bb_task_base_test_reset();
    // CONFIG_BB_TASK_BASE_MAX pinned to 8 for host tests (platformio.ini).
    int handles[9];
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch_or_insert(&handles[i], "t", 1));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_task_base_touch_or_insert(&handles[8], "overflow", 1));
}

// Race reproduction (HIGH finding): a concurrent bb_task_create()/
// bb_task_registry_register() inserting the REAL entry for `handle` in the
// gap between a non-atomic touch() + conditional upsert() must not be
// clobbered. bb_task_base_test_arm_race_insert() fires that competing
// insert from INSIDE touch_or_insert()'s own critical section, proving the
// atomic path re-checks presence before writing its placeholder instead of
// trusting its earlier "not found" snapshot.
void test_bb_task_base_touch_or_insert_race_does_not_clobber_real_entry(void)
{
    bb_task_base_test_reset();
    int fake;

    bb_task_base_test_arm_race_insert(&fake, "real_worker", 8192, true);
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch_or_insert(&fake, "scan_name", 3));

    foreach_snapshot_t snap = { 0 };
    bb_task_base_foreach(foreach_snapshot_cb, &snap);
    TEST_ASSERT_EQUAL_INT(1, snap.calls);
    TEST_ASSERT_EQUAL_STRING("real_worker", snap.last_name);
    TEST_ASSERT_EQUAL_UINT32(8192, snap.last_stack_bytes);
    TEST_ASSERT_TRUE(snap.last_wdt_arm);
}

// Race hook fires but the pool is already full when it tries its own
// insert -- exercises the race-injection's own "no free slot" branch (the
// hook is best-effort; it must not crash or corrupt state when the table
// it is racing against has no room either).
void test_bb_task_base_touch_or_insert_race_pool_full_is_noop(void)
{
    bb_task_base_test_reset();
    int handles[8];
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&handles[i], "t", 1, false));
    }

    int fake;
    bb_task_base_test_arm_race_insert(&fake, "race_worker", 1024, true);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_task_base_touch_or_insert(&fake, "scan_name", 1));
}

// ---------------------------------------------------------------------------
// bb_task_base_count / _capacity / _dropped -- B1-601 re-scope (occupancy
// accessors, mirror bb_task_registry_count/_capacity/_dropped).
// ---------------------------------------------------------------------------

void test_bb_task_base_capacity_returns_max_cap(void)
{
    TEST_ASSERT_EQUAL_UINT16(BB_TASK_BASE_MAX_CAP, bb_task_base_capacity());
}

void test_bb_task_base_count_reflects_live_upserts(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL_UINT16(0, bb_task_base_count());

    int a, b;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&a, "a", 1, false));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_base_count());
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&b, "b", 1, false));
    TEST_ASSERT_EQUAL_UINT16(2, bb_task_base_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_remove(&a));
    TEST_ASSERT_EQUAL_UINT16(1, bb_task_base_count());
}

void test_bb_task_base_dropped_zero_before_any_overflow(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL_UINT32(0, bb_task_base_dropped());
}

void test_bb_task_base_dropped_not_incremented_on_success(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fake, "t", 1, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch_or_insert(&fake, "t", 1));
    TEST_ASSERT_EQUAL_UINT32(0, bb_task_base_dropped());
}

void test_bb_task_base_dropped_increments_on_upsert_overflow(void)
{
    bb_task_base_test_reset();
    // CONFIG_BB_TASK_BASE_MAX pinned to 8 for host tests (platformio.ini).
    int handles[9];
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&handles[i], "t", 1, false));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_task_base_upsert(&handles[8], "overflow", 1, false));
    TEST_ASSERT_EQUAL_UINT32(1, bb_task_base_dropped());
    TEST_ASSERT_EQUAL_UINT16(8, bb_task_base_count());
}

void test_bb_task_base_dropped_increments_on_touch_or_insert_overflow(void)
{
    bb_task_base_test_reset();
    int handles[9];
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_task_base_touch_or_insert(&handles[i], "t", 1));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_task_base_touch_or_insert(&handles[8], "overflow", 1));
    TEST_ASSERT_EQUAL_UINT32(1, bb_task_base_dropped());
}

// ---------------------------------------------------------------------------
// bb_task_create / bb_task_deregister (host stub path)
// ---------------------------------------------------------------------------

void test_bb_task_create_null_cfg_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    void *handle = (void *)0x1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_create(NULL, &handle));
    TEST_ASSERT_NULL(handle);
}

void test_bb_task_create_invalid_config_propagates_resolve_error(void)
{
    bb_task_base_test_reset();
    bb_task_config_t cfg = { .entry = NULL, .name = "bad", .stack_bytes = 1024 };
    void *handle = (void *)0x1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_create(&cfg, &handle));
    TEST_ASSERT_NULL(handle);
}

void test_bb_task_create_success_upserts_base_entry(void)
{
    bb_task_base_test_reset();
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "worker", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm = true,
    };
    void *handle = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_create(&cfg, &handle));
    TEST_ASSERT_NOT_NULL(handle);

    foreach_capture_t cap = { 0 };
    bb_task_base_foreach(foreach_count_cb, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.calls);

    TEST_ASSERT_EQUAL(BB_OK, bb_task_deregister(handle));
}

void test_bb_task_create_out_handle_may_be_null(void)
{
    bb_task_base_test_reset();
    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "worker2", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_task_create(&cfg, NULL));
}

void test_bb_task_deregister_unregistered_returns_not_found(void)
{
    bb_task_base_test_reset();
    int fake;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_task_deregister(&fake));
}

void test_bb_task_deregister_null_returns_invalid_arg(void)
{
    bb_task_base_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_deregister(NULL));
}

// ---------------------------------------------------------------------------
// bb_task_create / bb_task_deregister — core-claim wiring (B1-1364 PR2).
// The host stub resolves against num_cores == 1 (see bb_task_host.c), so
// core 0 is a real (claimable) core and core 1 is not.
// ---------------------------------------------------------------------------

void test_bb_task_create_core_owning_claims_and_deregister_releases(void)
{
    bb_task_base_test_reset();
    bb_wdt_test_reset();

    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "owner", .stack_bytes = 2048,
        .core = 0, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    void *handle = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_create(&cfg, &handle));
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_claimed_core_mask());

    TEST_ASSERT_EQUAL(BB_OK, bb_task_deregister(handle));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());

    bb_wdt_test_reset();
}

// Two-owner rejection through the full bb_task_create() path (B1-1364 PR3,
// Finding 2): a second core_owning=true request targeting a core another
// live task already owns is rejected -- proving the guard fires from the
// real create() entry point, not only through bb_task_resolve() directly.
// The first owner's claim (and base registry entry) is left completely
// untouched by the rejected second attempt.
void test_bb_task_create_second_owning_claim_on_same_core_rejected(void)
{
    bb_task_base_test_reset();
    bb_wdt_test_reset();

    bb_task_config_t cfg1 = {
        .entry = noop_entry, .name = "owner1", .stack_bytes = 2048,
        .core = 0, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    void *handle1 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_create(&cfg1, &handle1));
    TEST_ASSERT_NOT_NULL(handle1);
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_claimed_core_mask());

    bb_task_config_t cfg2 = {
        .entry = noop_entry, .name = "owner2", .stack_bytes = 2048,
        .core = 0, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    void *handle2 = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_task_create(&cfg2, &handle2));
    TEST_ASSERT_NULL(handle2);
    // First owner's claim is completely unaffected by the rejected second
    // attempt -- still exactly one bit set.
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_claimed_core_mask());

    TEST_ASSERT_EQUAL(BB_OK, bb_task_deregister(handle1));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());

    bb_wdt_test_reset();
}

void test_bb_task_create_core_owning_degrades_to_noop_claim_on_unicore(void)
{
    bb_task_base_test_reset();
    bb_wdt_test_reset();

    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "owner2", .stack_bytes = 2048,
        .core = 1, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    void *handle = NULL;
    // core 1 does not exist on the host's simulated 1-core target --
    // bb_task_resolve()'s unicore clamp degrades this to BB_TASK_CORE_ANY,
    // so bb_task_create() must never call bb_wdt_claim_core() here.
    TEST_ASSERT_EQUAL(BB_OK, bb_task_create(&cfg, &handle));
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());

    TEST_ASSERT_EQUAL(BB_OK, bb_task_deregister(handle));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());

    bb_wdt_test_reset();
}

void test_bb_task_deregister_non_owning_task_does_not_touch_wdt(void)
{
    bb_task_base_test_reset();
    bb_wdt_test_reset();

    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "plain", .stack_bytes = 2048,
        .core = BB_TASK_CORE_ANY, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    void *handle = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_task_create(&cfg, &handle));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());

    TEST_ASSERT_EQUAL(BB_OK, bb_task_deregister(handle));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());
}

// Finding 2 (B1-1364 PR3 third-round review): a base-registry-full
// bb_task_base_upsert() failure must not leak the bb_wdt claim already
// taken above it -- bb_task_deregister() can never find an unregistered
// handle to release later (it looks the handle up in the same registry
// bb_task_base_upsert() failed to insert into). Task creation itself still
// succeeds (BB_OK, non-NULL handle) -- only the diagnostics registry entry
// and the WDT claim are affected.
void test_bb_task_create_core_owning_releases_claim_when_base_registry_full(void)
{
    bb_task_base_test_reset();
    bb_wdt_test_reset();

    // CONFIG_BB_TASK_BASE_MAX pinned to 8 for host tests (platformio.ini) --
    // fill every slot with unrelated (non-owning) entries first.
    int fillers[8];
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_task_base_upsert(&fillers[i], "filler", 1, false));
    }

    bb_task_config_t cfg = {
        .entry = noop_entry, .name = "owner", .stack_bytes = 2048,
        .core = 0, .core_owning = true, .backing = BB_TASK_BACKING_DYNAMIC,
    };
    void *handle = NULL;
    // bb_task_create() still succeeds -- the host fake-handle path already
    // ran before the base-registry upsert is attempted -- but the registry
    // is full, so bb_task_base_upsert() returns BB_ERR_NO_SPACE and the
    // claim taken above must be released rather than leaked.
    TEST_ASSERT_EQUAL(BB_OK, bb_task_create(&cfg, &handle));
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());

    bb_task_base_test_reset();
    bb_wdt_test_reset();
}

// ---------------------------------------------------------------------------
// bb_task_delay_ticks_for_ms -- pure ms->ticks conversion (B1-1350). This is
// the portable, coverage-gated logic every branch of the tick-truncation
// decision lives in; bb_task_delay_ms()'s espidf shell is a thin
// vTaskDelay(bb_task_delay_ticks_for_ms(ms, configTICK_RATE_HZ)) wrapper
// that can only be exercised by compiling for espidf (no FreeRTOS scheduler
// on host) -- see the bb_task_delay_ms/_yield host-stub tests below for what
// IS host-testable about that wrapper (linkage + no-crash only).
// ---------------------------------------------------------------------------

void test_bb_task_delay_ticks_for_ms_zero_ms_returns_zero_ticks(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_task_delay_ticks_for_ms(0, 100));
}

void test_bb_task_delay_ticks_for_ms_zero_tick_rate_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_task_delay_ticks_for_ms(50, 0));
}

// The tick-truncation guarantee: at a 100 Hz tick rate (10 ms/tick), 5 ms
// truncates to 0 ticks via plain integer division (5*100/1000 == 0) -- the
// helper must round this UP to the guaranteed minimum of 1 tick rather than
// silently returning "don't delay at all" for a caller that asked for a
// positive delay.
void test_bb_task_delay_ticks_for_ms_truncation_clamped_to_minimum_one_tick(void)
{
    TEST_ASSERT_EQUAL_UINT32(1, bb_task_delay_ticks_for_ms(5, 100));
}

void test_bb_task_delay_ticks_for_ms_exact_conversion_no_truncation(void)
{
    TEST_ASSERT_EQUAL_UINT32(100, bb_task_delay_ticks_for_ms(1000, 100));
}

// Ordinary (non-zero-truncating) integer division still applies once the
// result is >= 1 tick -- distinguishes plain truncation from the
// minimum-one-tick clamp exercised above.
void test_bb_task_delay_ticks_for_ms_truncates_toward_zero_above_one_tick(void)
{
    TEST_ASSERT_EQUAL_UINT32(2, bb_task_delay_ticks_for_ms(25, 100));
}

// An extreme (ms, tick_rate_hz) pair whose product overflows uint32_t must
// clamp to UINT32_MAX rather than silently wrapping to a small tick count.
void test_bb_task_delay_ticks_for_ms_clamps_to_uint32_max_on_overflow(void)
{
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, bb_task_delay_ticks_for_ms(UINT32_MAX, 2000));
}

// ---------------------------------------------------------------------------
// bb_task_delay_ms / bb_task_yield -- host stub (B1-1350). No real FreeRTOS
// scheduler exists on host, so these are no-ops: this proves call sites
// compile/link against the host stub and don't crash, NOT real blocking or
// yield timing behaviour (that only exists in the espidf shell, verified by
// compiling for espidf -- see platform/espidf/bb_task/bb_task_espidf.c).
// ---------------------------------------------------------------------------

void test_bb_task_delay_ms_host_stub_does_not_crash(void)
{
    bb_task_delay_ms(0);
    bb_task_delay_ms(50);  // must not crash or block
}

void test_bb_task_yield_host_stub_does_not_crash(void)
{
    bb_task_yield();  // must not crash
}
