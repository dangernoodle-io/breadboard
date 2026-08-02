// bb_task — host stub for bb_task_create()/bb_task_delay_ms()/
// bb_task_yield(). No real FreeRTOS task or scheduler exists on host;
// bb_task_create() fabricates a distinct non-NULL handle per call (no real
// thread) so the create() path still exercises bb_task_resolve() and
// bb_task_base_upsert() in host tests, mirroring how bb_task_registry's
// self-registration is host-testable via test_seed rather than a real
// TaskHandle_t. bb_task_delay_ms()/bb_task_yield() are no-ops (see bb_task.h).
#include "bb_task.h"
#include "bb_wdt.h"

#include <stddef.h>

// Fake "handle" pool — distinct addresses per call, no real task/thread.
// Sized generously; bb_task_create is not expected to be called at high
// volume in host tests.
#define BB_TASK_HOST_FAKE_HANDLES_MAX 32
static int s_fake_handles[BB_TASK_HOST_FAKE_HANDLES_MAX];
static int s_fake_handle_next = 0;

bb_err_t bb_task_create(const bb_task_config_t *cfg, void **out_handle)
{
    if (out_handle) {
        *out_handle = NULL;
    }
    if (!cfg) {
        return BB_ERR_INVALID_ARG;
    }

    // Host builds have exactly one "core" -- no affinity clamp to exercise
    // beyond what bb_task_resolve()'s own host tests already cover with an
    // explicit num_cores argument.
    bb_task_resolved_t resolved;
    // bb_wdt_claimed_core_mask() is read here (the platform shell), not
    // inside bb_task_resolve() -- keeps the resolver a pure function of its
    // arguments (B1-1356). See bb_task.h's bb_task_resolve() doc.
    bb_err_t err = bb_task_resolve(cfg, 1, bb_wdt_claimed_core_mask(), &resolved);
    if (err != BB_OK) {
        return err;
    }

    // core_owning claim (B1-1364 PR2/PR3-fix) -- same rule as the espidf
    // shell (platform/espidf/bb_task/bb_task_espidf.c): only a REAL
    // resolved core is ever claimed, via the atomic
    // bb_wdt_claim_core_exclusive() (not the plain, permissive
    // bb_wdt_claim_core()) -- see that file's comment for why the
    // resolve-time claimed_core_mask check alone does not close the
    // concurrent-double-claim race. On host, num_cores is always 1 above,
    // so bb_task_resolve()'s unicore clamp degrades any core-1 owning
    // request to BB_TASK_CORE_ANY -- core 0 is the only core an owning
    // claim can land on here.
    bool owning_claim = cfg->core_owning && resolved.core != BB_TASK_CORE_ANY;
    if (owning_claim) {
        bb_err_t claim_rc = bb_wdt_claim_core_exclusive(resolved.core);
        // LCOV_EXCL_START -- not host-reproducible: resolve()'s own
        // claimed_core_mask fast-path check above reads the SAME
        // bb_wdt_claimed_core_mask() snapshot this claim would race
        // against, so on a single-threaded host it always agrees with the
        // real-time state bb_wdt_claim_core_exclusive() observes here --
        // this branch only fires under a genuine concurrent double-claim
        // (see bb_wdt.h's "Exclusivity of OWNERSHIP" note and
        // test_bb_wdt_claim_core_exclusive_rejects_already_claimed(),
        // which drives the atomic primitive's reject path directly instead).
        if (claim_rc != BB_OK) {
            return claim_rc;
        }
        // LCOV_EXCL_STOP
    }

    void *handle = &s_fake_handles[s_fake_handle_next % BB_TASK_HOST_FAKE_HANDLES_MAX];
    s_fake_handle_next++;

    // No real FreeRTOS task/StackType_t on host -- record stack_bytes
    // as-is (no sizeof(StackType_t) conversion; that only happens in the
    // espidf shell).
    bb_err_t base_rc = bb_task_base_upsert(handle, cfg->name, resolved.stack_bytes, cfg->wdt_arm);
    if (base_rc == BB_OK) {
        base_rc = bb_task_base_set_core_owning(handle, resolved.core, owning_claim);
    }
    if (owning_claim && base_rc != BB_OK) {
        // Same leak-prevention rationale as the espidf shell -- see its
        // comment: bb_task_deregister() can never find an unregistered
        // handle to release its claim later.
        bb_wdt_release_core(resolved.core);
    }

    if (out_handle) {
        *out_handle = handle;
    }
    return BB_OK;
}

// No real FreeRTOS scheduler on host -- no-ops that return immediately
// rather than sleeping/yielding a real thread. See bb_task.h for the full
// host-stub rationale (mirrors bb_task_create() above).
void bb_task_delay_ms(uint32_t ms)
{
    (void)ms;
}

void bb_task_yield(void)
{
}
