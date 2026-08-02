#include "unity.h"
#include "bb_wdt.h"
#include "bb_wdt_test.h"
#include "bb_task.h"
#include <stdint.h>
#include <stdbool.h>

/* BB_WDT_CORE_ANY and BB_TASK_CORE_ANY are independently defined (bb_wdt
 * does not, and must not, take a component dependency on bb_task -- a later
 * PR wires bb_task -> bb_wdt, and a bb_wdt -> bb_task edge would make that a
 * cycle). This tripwire is compiled on every native test run, so a drift
 * between the two sentinels is caught here rather than left to guard
 * nothing until the two components are actually wired together. */
_Static_assert(BB_WDT_CORE_ANY == BB_TASK_CORE_ANY,
                "BB_WDT_CORE_ANY must equal BB_TASK_CORE_ANY -- both are the "
                "same 'no explicit core' sentinel, kept as two independent "
                "#defines only to avoid a premature bb_wdt->bb_task dependency");

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

typedef struct {
    int      calls_until_success; /* -1 = never succeeds */
    int      call_count;
    uint32_t last_ms;
} fake_wait_ctx_t;

static bool fake_try_wait(void *ctx, uint32_t ms)
{
    fake_wait_ctx_t *c = (fake_wait_ctx_t *)ctx;
    c->last_ms = ms;
    c->call_count++;
    if (c->calls_until_success < 0) return false;
    if (c->call_count >= c->calls_until_success) return true;
    return false;
}

/* -------------------------------------------------------------------------
 * bb_wdt_park_wait: unsubscribe → wait → resubscribe → feed
 * ---------------------------------------------------------------------- */

void test_bb_wdt_park_wait_resume_unsubscribes_and_resubscribes(void)
{
    bb_wdt_test_reset();
    /* resumes on the single wait */
    fake_wait_ctx_t ctx = { .calls_until_success = 1, .call_count = 0 };
    bool result = bb_wdt_park_wait(fake_try_wait, &ctx, 5000, 1000);
    TEST_ASSERT_TRUE(result);
    /* try_wait called exactly once with the full budget (no slicing) */
    TEST_ASSERT_EQUAL_INT(1, ctx.call_count);
    TEST_ASSERT_EQUAL_UINT32(5000, ctx.last_ms);
    /* removed from the WDT for the park, re-added and fed on resume */
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_feed_count());
}

void test_bb_wdt_park_wait_timeout_still_resubscribes(void)
{
    bb_wdt_test_reset();
    /* never resumes → timeout */
    fake_wait_ctx_t ctx = { .calls_until_success = -1, .call_count = 0 };
    bool result = bb_wdt_park_wait(fake_try_wait, &ctx, 3000, 1000);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL_INT(1, ctx.call_count);
    TEST_ASSERT_EQUAL_UINT32(3000, ctx.last_ms);
    /* must re-add to the WDT even on timeout, else the task is never watched again */
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_feed_count());
}

void test_bb_wdt_park_wait_null_try_wait_returns_false(void)
{
    bb_wdt_test_reset();
    bool result = bb_wdt_park_wait(NULL, NULL, 1000, 100);
    TEST_ASSERT_FALSE(result);
    /* returns before touching the WDT */
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_test_subscribe_count());
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_test_feed_count());
}

/* -------------------------------------------------------------------------
 * Host no-op backend: feed/subscribe/unsubscribe counters
 * ---------------------------------------------------------------------- */

void test_bb_wdt_feed_increments_counter(void)
{
    bb_wdt_test_reset();
    bb_wdt_task_feed();
    bb_wdt_task_feed();
    TEST_ASSERT_EQUAL_INT(2, bb_wdt_test_feed_count());
}

void test_bb_wdt_subscribe_increments_counter(void)
{
    bb_wdt_test_reset();
    bb_err_t rc = bb_wdt_task_subscribe();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
}

void test_bb_wdt_unsubscribe_increments_counter(void)
{
    bb_wdt_test_reset();
    bb_err_t rc = bb_wdt_task_unsubscribe();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
}

void test_bb_wdt_set_timeout_noop_on_host(void)
{
    /* just verifies it doesn't crash */
    bb_wdt_set_timeout(60);
    bb_wdt_extend_begin(300);
    bb_wdt_extend_end();
    TEST_PASS();
}

/* -------------------------------------------------------------------------
 * bb_wdt_task_subscribe_handle / bb_wdt_task_unsubscribe_handle
 * (B1-1043: arbitrary-task-handle subscribe API)
 * ---------------------------------------------------------------------- */

void test_bb_wdt_subscribe_handle_increments_counter(void)
{
    bb_wdt_test_reset();
    int fake_handle;
    bb_err_t rc = bb_wdt_task_subscribe_handle(&fake_handle);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
    /* the exact handle pointer must be routed through, not dropped */
    TEST_ASSERT_EQUAL_PTR(&fake_handle, bb_wdt_test_last_subscribe_handle());
}

void test_bb_wdt_unsubscribe_handle_increments_counter(void)
{
    bb_wdt_test_reset();
    int fake_handle;
    bb_err_t rc = bb_wdt_task_unsubscribe_handle(&fake_handle);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_EQUAL_PTR(&fake_handle, bb_wdt_test_last_unsubscribe_handle());
}

void test_bb_wdt_subscribe_handle_null_is_self_subscribe(void)
{
    /* NULL handle routes through the same counter as the self API — this
     * is the "thin wrapper" contract: bb_wdt_task_subscribe() ==
     * bb_wdt_task_subscribe_handle(NULL). */
    bb_wdt_test_reset();
    bb_err_t rc = bb_wdt_task_subscribe_handle(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
    TEST_ASSERT_NULL(bb_wdt_test_last_subscribe_handle());
}

void test_bb_wdt_unsubscribe_handle_null_is_self_unsubscribe(void)
{
    bb_wdt_test_reset();
    bb_err_t rc = bb_wdt_task_unsubscribe_handle(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_NULL(bb_wdt_test_last_unsubscribe_handle());
}

void test_bb_wdt_task_subscribe_routes_through_handle_api(void)
{
    /* bb_wdt_task_subscribe()/unsubscribe() are thin wrappers over the
     * handle API passing NULL — same observable counters either way. */
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_task_subscribe());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_task_unsubscribe());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
    /* a regression that ignored the caller's handle and always
     * self-subscribed would still pass the counter-only assertions above;
     * this pins the NULL-routing explicitly. */
    TEST_ASSERT_NULL(bb_wdt_test_last_subscribe_handle());
    TEST_ASSERT_NULL(bb_wdt_test_last_unsubscribe_handle());
}

void test_bb_wdt_subscribe_handle_distinguishes_from_null(void)
{
    /* real regression guard: a non-NULL handle call must record THAT
     * pointer, not silently collapse to the self (NULL) path. */
    bb_wdt_test_reset();
    int fake_handle;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_task_subscribe_handle(&fake_handle));
    TEST_ASSERT_NOT_NULL(bb_wdt_test_last_subscribe_handle());
    TEST_ASSERT_EQUAL_PTR(&fake_handle, bb_wdt_test_last_subscribe_handle());

    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_task_subscribe_handle(NULL));
    TEST_ASSERT_NULL(bb_wdt_test_last_subscribe_handle());
}

/* -------------------------------------------------------------------------
 * bb_wdt_claim_core / bb_wdt_release_core: bookkeeping
 * (B1-1364 PR1 -- core-claim mechanism, DORMANT: no in-tree caller yet)
 * ---------------------------------------------------------------------- */

void test_bb_wdt_claim_core_sets_bit(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_1_sets_bit(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(1));
    TEST_ASSERT_EQUAL_UINT32(1U << 1, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_both_accumulates(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(1));
    TEST_ASSERT_EQUAL_UINT32((1U << 0) | (1U << 1), bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_idempotent_same_core(void)
{
    /* claims accumulate into a bitmask -- idle-check suppression is not an
     * exclusive resource, so re-claiming an already-claimed core is a
     * success, not a conflict. */
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_invalid_arg(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_wdt_claim_core(2));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_wdt_claim_core(-2));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_release_core_clears_bit(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(1));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_release_core(0));
    TEST_ASSERT_EQUAL_UINT32(1U << 1, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_release_core_1_clears_bit(void)
{
    /* covers the core==1 valid-arg branch of bb_wdt_release_core()'s
     * validation (core != 0 && core != 1), distinct from the core==0 case
     * above. */
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(1));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_release_core(1));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_release_core_unclaimed_is_ok(void)
{
    /* tolerant: releasing a core that was never claimed is not an error. */
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_release_core(0));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_release_core_invalid_arg(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_wdt_release_core(3));
}

/* -------------------------------------------------------------------------
 * bb_wdt_claim_core_exclusive: atomic check-and-set (B1-1364 PR3-fix,
 * TOCTOU-race Finding 1). Real interleaved concurrency is NOT
 * host-reproducible (host tests are single-threaded) -- these tests drive
 * the atomic primitive's own reject path directly (a pre-set mask standing
 * in for "a competing claim already landed") rather than exercising a true
 * concurrent race; see bb_wdt.h for the full rationale.
 * ---------------------------------------------------------------------- */

void test_bb_wdt_claim_core_exclusive_sets_bit_on_first_claim(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core_exclusive(0));
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_exclusive_1_sets_bit_on_first_claim(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core_exclusive(1));
    TEST_ASSERT_EQUAL_UINT32(1U << 1, bb_wdt_claimed_core_mask());
}

// Delete-the-call proof target: this is the reject path that a non-atomic
// (or always-succeeding) implementation would fail to produce.
void test_bb_wdt_claim_core_exclusive_rejects_already_claimed(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core_exclusive(0));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, bb_wdt_claim_core_exclusive(0));
    /* the losing call must not have altered the mask. */
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_exclusive_rejects_claimed_via_plain_claim(void)
{
    /* a core claimed through the plain (non-exclusive)
     * bb_wdt_claim_core() -- e.g. by a caller that bypasses
     * bb_task_create() -- is still seen by the exclusive path: both share
     * the same underlying bitmask. */
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(1));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, bb_wdt_claim_core_exclusive(1));
    TEST_ASSERT_EQUAL_UINT32(1U << 1, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_exclusive_other_core_still_succeeds(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core_exclusive(0));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core_exclusive(1));
    TEST_ASSERT_EQUAL_UINT32((1U << 0) | (1U << 1), bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_exclusive_invalid_arg(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_wdt_claim_core_exclusive(2));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_wdt_claim_core_exclusive(-2));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_claim_core_exclusive_after_release_succeeds_again(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core_exclusive(0));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_release_core(0));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core_exclusive(0));
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_claimed_core_mask());
}

void test_bb_wdt_test_reset_clears_claims(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(1));
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_claimed_core_mask());
}

/* -------------------------------------------------------------------------
 * bb_wdt_derive_idle_mask: PURE mask-union math
 * ---------------------------------------------------------------------- */

void test_bb_wdt_derive_idle_mask_zero_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_derive_idle_mask(0, 0));
}

void test_bb_wdt_derive_idle_mask_kconfig_only(void)
{
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_derive_idle_mask(1U << 0, 0));
}

void test_bb_wdt_derive_idle_mask_claimed_only(void)
{
    TEST_ASSERT_EQUAL_UINT32(1U << 1, bb_wdt_derive_idle_mask(0, 1U << 1));
}

void test_bb_wdt_derive_idle_mask_union_never_overrides(void)
{
    /* OR, never override: a claim on core 1 must not drop the board's
     * Kconfig-configured excuse on core 0, and vice versa. */
    TEST_ASSERT_EQUAL_UINT32((1U << 0) | (1U << 1),
                              bb_wdt_derive_idle_mask(1U << 0, 1U << 1));
}

void test_bb_wdt_derive_idle_mask_both_same_bit_is_idempotent(void)
{
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_derive_idle_mask(1U << 0, 1U << 0));
}

void test_bb_wdt_derive_idle_mask_both_full(void)
{
    uint32_t both = (1U << 0) | (1U << 1);
    TEST_ASSERT_EQUAL_UINT32(both, bb_wdt_derive_idle_mask(both, both));
}

/* -------------------------------------------------------------------------
 * bb_wdt_clamp_idle_mask: PURE clamp math (Finding 1, B1-1364 PR1 review)
 * ---------------------------------------------------------------------- */

void test_bb_wdt_clamp_idle_mask_dual_core_unchanged(void)
{
    uint32_t both = (1U << 0) | (1U << 1);
    TEST_ASSERT_EQUAL_UINT32(both, bb_wdt_clamp_idle_mask(both, 2));
}

void test_bb_wdt_clamp_idle_mask_unicore_drops_bit1(void)
{
    /* the exact scenario esp_task_wdt_reconfigure() would otherwise reject:
     * bit 1 set on a 1-core target. */
    uint32_t both = (1U << 0) | (1U << 1);
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_clamp_idle_mask(both, 1));
}

void test_bb_wdt_clamp_idle_mask_unicore_only_bit1_clamps_to_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, bb_wdt_clamp_idle_mask(1U << 1, 1));
}

void test_bb_wdt_clamp_idle_mask_zero_cores_clamps_to_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, bb_wdt_clamp_idle_mask((1U << 0) | (1U << 1), 0));
}

void test_bb_wdt_clamp_idle_mask_negative_cores_clamps_to_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, bb_wdt_clamp_idle_mask(1U << 0, -1));
}

void test_bb_wdt_clamp_idle_mask_large_num_cores_unchanged(void)
{
    /* num_cores >= 32: no uint32_t bit can be out of range; also guards
     * against `1U << 32` (undefined behavior). */
    uint32_t both = (1U << 0) | (1U << 1);
    TEST_ASSERT_EQUAL_UINT32(both, bb_wdt_clamp_idle_mask(both, 32));
}

/* -------------------------------------------------------------------------
 * Finding 1 (B1-1364 PR1 review): a core-1 claim on a 1-core configuration
 * must never produce an out-of-range idle_core_mask, and must not poison a
 * subsequent reconfigure. Exercised through the same do_reconfigure() path
 * the ESP-IDF backend uses (see bb_wdt_test_set_num_cores()).
 * ---------------------------------------------------------------------- */

void test_bb_wdt_claim_core_1_on_unicore_never_produces_out_of_range_mask(void)
{
    bb_wdt_test_reset();
    bb_wdt_test_set_num_cores(1);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(1));
    /* bookkeeping still records the claim (bb_wdt_claimed_core_mask() and
     * bb_wdt_steer_core() see it -- claiming is not validated against the
     * real core count)... */
    TEST_ASSERT_EQUAL_UINT32(1U << 1, bb_wdt_claimed_core_mask());
    /* ...but the mask actually handed to reconfigure never sets an
     * out-of-range bit -- esp_task_wdt_reconfigure() would reject a mask
     * with bit 1 set on a 1-core target. */
    TEST_ASSERT_EQUAL_UINT32(0U, bb_wdt_test_last_reconfigured_mask());
}

void test_bb_wdt_claim_core_1_on_unicore_does_not_poison_later_reconfigure(void)
{
    bb_wdt_test_reset();
    bb_wdt_test_set_num_cores(1);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(1));
    TEST_ASSERT_EQUAL_UINT32(0U, bb_wdt_test_last_reconfigured_mask());

    /* a legitimate core-0 claim afterward must still apply cleanly -- the
     * out-of-range claim must never persist/poison a later reconfigure
     * (the historical bug this PR fixes: esp_task_wdt_reconfigure()
     * rejecting the out-of-range mask would silently drop this too). */
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_test_last_reconfigured_mask());

    /* an unrelated bb_wdt_set_timeout() call afterward must also still
     * apply -- proving the earlier out-of-range claim never poisons the
     * shared reconfigure path for a caller with no relationship to it. */
    bb_wdt_set_timeout(60);
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_test_last_reconfigured_mask());
}

/* -------------------------------------------------------------------------
 * bb_wdt_set_timeout() must NOT clobber a live claim -- the highest-value
 * test in this PR (B1-1364 PR1). Proves bb_wdt_set_timeout() and
 * bb_wdt_claim_core()/bb_wdt_release_core() share ONE derivation path, not
 * two that can silently diverge.
 * ---------------------------------------------------------------------- */

void test_bb_wdt_set_timeout_does_not_clobber_claim(void)
{
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_test_last_reconfigured_mask());

    /* an unrelated later bb_wdt_set_timeout() (e.g. the
     * bb_wdt_extend_begin()/extend_end() pair) must still apply the live
     * claim, not silently re-derive from Kconfig alone. */
    bb_wdt_set_timeout(60);
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_test_last_reconfigured_mask());
}

void test_bb_wdt_release_core_reconfigures_immediately(void)
{
    /* claim/release each trigger their own reconfigure -- no observable
     * "declared but not applied" window. */
    bb_wdt_test_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_claim_core(0));
    TEST_ASSERT_EQUAL_UINT32(1U << 0, bb_wdt_test_last_reconfigured_mask());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_wdt_release_core(0));
    TEST_ASSERT_EQUAL_UINT32(0, bb_wdt_test_last_reconfigured_mask());
}

/* -------------------------------------------------------------------------
 * bb_wdt_steer_core: PURE
 * ---------------------------------------------------------------------- */

void test_bb_wdt_steer_core_explicit_pin_0_passes_through(void)
{
    /* an explicit pin always wins over steering, even if that core is
     * claimed by someone else. */
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_steer_core(0, 1U << 1, 2));
}

void test_bb_wdt_steer_core_explicit_pin_1_passes_through(void)
{
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_steer_core(1, 1U << 0, 2));
}

void test_bb_wdt_steer_core_any_steers_away_from_claimed_core0(void)
{
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_steer_core(BB_WDT_CORE_ANY, 1U << 0, 2));
}

void test_bb_wdt_steer_core_any_steers_away_from_claimed_core1(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_steer_core(BB_WDT_CORE_ANY, 1U << 1, 2));
}

void test_bb_wdt_steer_core_any_zero_claimed_unchanged(void)
{
    TEST_ASSERT_EQUAL_INT(BB_WDT_CORE_ANY, bb_wdt_steer_core(BB_WDT_CORE_ANY, 0, 2));
}

void test_bb_wdt_steer_core_any_both_claimed_unchanged(void)
{
    /* both claimed -- nothing to steer toward. */
    uint32_t both = (1U << 0) | (1U << 1);
    TEST_ASSERT_EQUAL_INT(BB_WDT_CORE_ANY, bb_wdt_steer_core(BB_WDT_CORE_ANY, both, 2));
}

void test_bb_wdt_steer_core_unicore_any_unchanged(void)
{
    /* num_cores < 2: nothing to steer away from, even if a claim is
     * (nonsensically) recorded. */
    TEST_ASSERT_EQUAL_INT(BB_WDT_CORE_ANY, bb_wdt_steer_core(BB_WDT_CORE_ANY, 1U << 0, 1));
}

void test_bb_wdt_steer_core_unicore_explicit_pin_unchanged(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_steer_core(0, 0, 1));
}

void test_bb_wdt_steer_core_other_value_passes_through(void)
{
    /* any requested value other than 0, 1, or BB_WDT_CORE_ANY (a caller
     * passing a raw, non-sentinel value) is passed through unchanged --
     * only BB_WDT_CORE_ANY is ever steered. */
    TEST_ASSERT_EQUAL_INT(5, bb_wdt_steer_core(5, 1U << 0, 2));
}
