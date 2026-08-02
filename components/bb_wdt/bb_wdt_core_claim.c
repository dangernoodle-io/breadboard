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

uint32_t bb_wdt_derive_idle_mask(uint32_t kconfig_default_mask, uint32_t claimed_mask)
{
    // OR, never override: a claim only ever ADDS an excused core, it never
    // removes a check the board owner asked for via Kconfig.
    return kconfig_default_mask | claimed_mask;
}

uint32_t bb_wdt_clamp_idle_mask(uint32_t mask, int num_cores)
{
    if (num_cores <= 0) {
        return 0U;
    }
    if (num_cores >= 32) {
        // No uint32_t bit can be out of range for a 32+-core target; also
        // guards against `1U << 32`, which is undefined behavior in C.
        return mask;
    }
    return mask & ((1U << (uint32_t)num_cores) - 1U);
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
