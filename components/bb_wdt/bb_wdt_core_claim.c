// bb_wdt core-claim mechanism (B1-1364 PR1). Portable -- compiled on both
// host (tests) and ESP-IDF (same object file via CMake SRCS), same as
// bb_wdt_park_wait.c alongside it. See bb_wdt.h's "Core-claim mechanism"
// section for the full contract.

#include "bb_wdt.h"
#include "bb_wdt_priv.h"
#include "bb_lock.h"
#include "bb_lock_once.h"

static bb_once_t s_lock_once = BB_ONCE_INIT;
static bb_lock_t s_lock;
static uint32_t  s_claimed_mask = 0;

// Lazily bb_lock_init() s_lock exactly once (mirrors bb_claim.c's identical
// idiom) -- no explicit init call needed, no platform mutex type appears in
// this file. This is a cold path (task-creation time), so no lock-free
// requirement; the lightest lock the shared claim/release read-modify-write
// genuinely needs.
static inline bb_err_t ensure_lock(void)
{
    bb_lock_config_t cfg = { .name = "bb_wdt_claim" };
    return bb_lock_once_ensure(&s_lock_once, &cfg, &s_lock);
}

bb_err_t bb_wdt_claim_core(int core)
{
    if (core != 0 && core != 1) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t lock_rc = ensure_lock();
    // LCOV_EXCL_START -- bb_lock_init() failure is not host-reproducible
    // (same rationale as bb_claim.c's identical comment). Defensive path,
    // not a real branch the test suite can drive.
    if (lock_rc != BB_OK) {
        return lock_rc;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_lock);
    s_claimed_mask |= (1U << (uint32_t)core);
    bb_lock_unlock(&s_lock);

    // Reapply runs OUTSIDE the critical section above -- the guarantee here
    // is convergence, not atomicity. do_reconfigure() re-derives the mask
    // fresh from bb_wdt_claimed_core_mask() rather than taking a snapshot
    // passed in, so a racing claim/release/set_timeout between the unlock
    // above and this call is never lost: the loser's reapply simply re-reads
    // whatever the mask is by the time it runs (last write wins, always a
    // valid state, never a stale one).
    bb_wdt_priv_reapply();
    return BB_OK;
}

bb_err_t bb_wdt_claim_core_exclusive(int core)
{
    if (core != 0 && core != 1) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t lock_rc = ensure_lock();
    // LCOV_EXCL_START -- see bb_wdt_claim_core()'s identical rationale.
    if (lock_rc != BB_OK) {
        return lock_rc;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_lock);
    uint32_t bit = (1U << (uint32_t)core);
    // Check-and-set under the SAME critical section -- this is what makes
    // the claim exclusive: bb_wdt_claim_core()'s accumulate semantics OR
    // the bit in unconditionally, so a check performed by a caller BEFORE
    // taking this lock (e.g. bb_task_resolve()'s claimed_core_mask
    // snapshot) can never be trusted alone -- two callers could both pass
    // that check and both reach here. Only one of them observes the bit
    // still clear inside this lock.
    if ((s_claimed_mask & bit) != 0U) {
        bb_lock_unlock(&s_lock);
        return BB_ERR_INVALID_STATE;
    }
    s_claimed_mask |= bit;
    bb_lock_unlock(&s_lock);

    // Reapply runs OUTSIDE the critical section -- same convergence (not
    // atomicity) rationale as bb_wdt_claim_core() above.
    bb_wdt_priv_reapply();
    return BB_OK;
}

bb_err_t bb_wdt_release_core(int core)
{
    if (core != 0 && core != 1) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t lock_rc = ensure_lock();
    // LCOV_EXCL_START -- see bb_wdt_claim_core()'s identical rationale.
    if (lock_rc != BB_OK) {
        return lock_rc;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_lock);
    s_claimed_mask &= ~(1U << (uint32_t)core);
    bb_lock_unlock(&s_lock);

    // See bb_wdt_claim_core()'s identical comment: convergence, not
    // atomicity -- do_reconfigure() re-derives the mask fresh.
    bb_wdt_priv_reapply();
    return BB_OK;
}

uint32_t bb_wdt_claimed_core_mask(void)
{
    bb_err_t lock_rc = ensure_lock();
    // LCOV_EXCL_START -- see bb_wdt_claim_core()'s identical rationale;
    // nothing could have been claimed either if the lock never initialized.
    if (lock_rc != BB_OK) {
        return 0U;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_lock);
    uint32_t mask = s_claimed_mask;
    bb_lock_unlock(&s_lock);
    return mask;
}

bb_wdt_excused_mask_t bb_wdt_derive_excused_mask(bb_wdt_excused_mask_t excused_seed,
                                                  bb_wdt_excused_mask_t claimed_mask)
{
    // UNION, never override -- both operands are EXCUSED polarity (see
    // bb_wdt.h's "Polarity hardening" note, B1-1408): a claim only ever
    // ADDS an excused core, it never removes an idle check the board owner
    // asked for via Kconfig.
    return (bb_wdt_excused_mask_t){ .bits = excused_seed.bits | claimed_mask.bits };
}

bb_wdt_excused_mask_t bb_wdt_clamp_core_mask(bb_wdt_excused_mask_t mask, int num_cores)
{
    if (num_cores <= 0) {
        return (bb_wdt_excused_mask_t){ .bits = 0U };
    }
    if (num_cores >= 32) {
        // No uint32_t bit can be out of range for a 32+-core target; also
        // guards against `1U << 32`, which is undefined behavior in C.
        return mask;
    }
    return (bb_wdt_excused_mask_t){ .bits = mask.bits & ((1U << (uint32_t)num_cores) - 1U) };
}

uint32_t bb_wdt_invert_core_mask(uint32_t mask, int num_cores)
{
    // full_mask(num_cores): reuse bb_wdt_clamp_core_mask()'s identical
    // range-truncation math rather than re-deriving it -- clamping
    // "all bits set" to num_cores IS full_mask(num_cores).
    uint32_t full = bb_wdt_clamp_core_mask((bb_wdt_excused_mask_t){ .bits = ~0U }, num_cores).bits;
    return full & ~mask;
}

int bb_wdt_steer_core(int requested, uint32_t claimed_mask, int num_cores)
{
    // An explicit pin always wins over steering.
    if (requested == 0 || requested == 1) {
        return requested;
    }
    if (requested != BB_WDT_CORE_ANY) {
        return requested;
    }
    // Unicore: nothing to steer away from.
    if (num_cores < 2) {
        return requested;
    }

    bool core0_claimed = (claimed_mask & (1U << 0)) != 0U;
    bool core1_claimed = (claimed_mask & (1U << 1)) != 0U;

    if (core0_claimed && !core1_claimed) {
        return 1;
    }
    if (core1_claimed && !core0_claimed) {
        return 0;
    }
    // Zero or both claimed: nothing to steer toward.
    return requested;
}

#ifdef BB_WDT_TESTING
void bb_wdt_priv_test_reset_claims(void)
{
    bb_err_t lock_rc = ensure_lock();
    // LCOV_EXCL_START -- see bb_wdt_claim_core()'s identical rationale.
    if (lock_rc != BB_OK) {
        return;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_lock);
    s_claimed_mask = 0;
    bb_lock_unlock(&s_lock);
}
#endif
