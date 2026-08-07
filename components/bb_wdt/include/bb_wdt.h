#pragma once

/*
 * bb_wdt — portable Task Watchdog Timer management.
 *
 * Consolidates live Task-WDT management (timeout reconfiguration,
 * task subscription, periodic feeding, and the WDT-friendly poll loop)
 * into a single component. ESP-IDF backend wraps esp_task_wdt_*; host
 * backend is no-op (with test-observable counters when BB_WDT_TESTING).
 *
 * Portability: this header contains no ESP-IDF or FreeRTOS types.
 */

#include "bb_core.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reconfigure the Task WDT timeout, preserving the idle-core mask and
 * panic-on-timeout policy from sdkconfig. ESP-IDF only; no-op on host.
 */
void bb_wdt_set_timeout(uint32_t timeout_s);

/*
 * Extend the Task WDT timeout to extended_s for a long blocking operation
 * (e.g. OTA flash). Mirrors bb_wdt_set_timeout(extended_s).
 */
void bb_wdt_extend_begin(uint32_t extended_s);

/*
 * Restore the Task WDT timeout to CONFIG_ESP_TASK_WDT_TIMEOUT_S.
 * Call after a long operation to return to the normal WDT window.
 */
void bb_wdt_extend_end(void);

/*
 * Config for bb_wdt_task_subscribe() (B1-1460) -- collapses the former
 * bb_wdt_task_subscribe()/bb_wdt_task_subscribe_handle() variant-ladder
 * pair into one entry point. `handle` is an opaque platform task handle
 * (TaskHandle_t on ESP-IDF); a zero-init struct (handle == NULL) subscribes
 * the CALLING task -- NULL is esp_task_wdt_add's own "current task" sentinel
 * (esp_task_wdt_add(NULL)), not a distinct "unset" state that needs its own
 * sentinel, so zero-init cfg reproduces the pre-collapse no-arg call exactly.
 */
typedef struct {
    void *handle;
} bb_wdt_task_subscribe_cfg_t;

/*
 * Subscribe a task to the Task WDT (esp_task_wdt_add(cfg->handle)).
 * `cfg->handle == NULL` subscribes the calling task. Single wrapper over
 * esp_task_wdt_add — callers must not call the raw ESP-IDF API directly.
 * Returns BB_OK on success, BB_ERR_INVALID_ARG if cfg is NULL, or a platform
 * error code on failure. No-op / returns BB_OK on host (given non-NULL cfg).
 */
bb_err_t bb_wdt_task_subscribe(const bb_wdt_task_subscribe_cfg_t *cfg);

/*
 * Config for bb_wdt_task_unsubscribe() (B1-1460) -- collapses the former
 * bb_wdt_task_unsubscribe()/bb_wdt_task_unsubscribe_handle() variant-ladder
 * pair into one entry point. `handle` is an opaque platform task handle
 * (TaskHandle_t on ESP-IDF); a zero-init struct (handle == NULL) unsubscribes
 * the CALLING task -- NULL is esp_task_wdt_delete's own "current task"
 * sentinel (esp_task_wdt_delete(NULL)), not a distinct "unset" state that
 * needs its own sentinel, so zero-init cfg reproduces the pre-collapse
 * no-arg call exactly.
 */
typedef struct {
    void *handle;
} bb_wdt_task_unsubscribe_cfg_t;

/*
 * Unsubscribe a task from the Task WDT (esp_task_wdt_delete(cfg->handle)).
 * `cfg->handle == NULL` unsubscribes the calling task. Single wrapper over
 * esp_task_wdt_delete — callers must not call the raw ESP-IDF API directly.
 * Returns BB_OK on success, BB_OK if not subscribed, BB_ERR_INVALID_ARG if
 * cfg is NULL, or a platform error code on failure. No-op / returns BB_OK
 * on host (given non-NULL cfg).
 */
bb_err_t bb_wdt_task_unsubscribe(const bb_wdt_task_unsubscribe_cfg_t *cfg);

/*
 * Feed the Task WDT for the calling task (esp_task_wdt_reset(NULL)).
 * No-op on host.
 */
void bb_wdt_task_feed(void);

/*
 * Portable WDT-friendly park.
 *
 * Removes the calling task from the Task WDT, blocks on try_wait(ctx, total_ms),
 * then re-adds the task and feeds it. Used to park a WDT-subscribed task (e.g.
 * mining/asic) across a long cooperative pause (OTA flash phase): on a single
 * core the parked task cannot get scheduled to feed, so it must leave the WDT
 * rather than try to feed it. The active task's own subscription plus the OTA
 * timeout extension cover the system while parked.
 *
 * Parameters:
 *   try_wait  — blocking predicate; returns true when the park ends (resume)
 *   ctx       — opaque context passed through to try_wait
 *   total_ms  — total wait budget in milliseconds (passed straight to try_wait)
 *   slice_ms  — retained for ABI compatibility; ignored
 *
 * Returns try_wait's result: true if resumed within total_ms, false on timeout.
 * Assumes the caller is a WDT-subscribed task.
 */
bool bb_wdt_park_wait(bool (*try_wait)(void *ctx, uint32_t ms),
                      void *ctx,
                      uint32_t total_ms,
                      uint32_t slice_ms);

/*
 * ---------------------------------------------------------------------------
 * Core-claim mechanism (B1-1364 PR1 -- core-claim primitive only).
 *
 * A core-pinned CPU-bound task starves that core's IDLE task, tripping the
 * Task WDT unless the idle check for that core is excused. Historically that
 * meant hand-flipping CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPUn per board -- a
 * manual global-polarity footgun that silently diverges from what a given
 * board composition actually pins. bb_wdt_claim_core() lets composition
 * declare "this core is claimed by a pinned task" and have the excused-idle
 * mask DERIVED from that declaration, instead of hand-set.
 *
 * DORMANT IN THIS PR: no in-tree caller yet. A later PR wires bb_task's
 * pinned-task creation path to call bb_wdt_claim_core()/bb_wdt_steer_core()
 * automatically; this PR ships the primitive + pure evaluators + host tests
 * only.
 *
 * Claims ACCUMULATE into an internal bitmask -- idle-check suppression is
 * not an exclusive resource, so claiming an already-claimed core (by the
 * same or a different caller) is an idempotent success at THIS layer, never
 * a conflict: bb_wdt_claim_core()/bb_wdt_release_core() have no per-owner
 * refcount, only a single shared bit per core, so a second claim on a core
 * is indistinguishable from a re-claim by the same owner, and a release
 * unconditionally clears the bit regardless of how many owners claimed it.
 * Every reconfigure (bb_wdt_set_timeout(), bb_wdt_claim_core(),
 * bb_wdt_release_core()) computes `excused_seed | claimed_mask`, entirely in
 * EXCUSED polarity (see "Polarity hardening" above) -- the claim only ever
 * ADDS an excused core, never removes an idle check the board owner asked
 * for via Kconfig on an unclaimed core. `excused_seed` is the board's
 * Kconfig-configured default CONVERTED into excused polarity once per
 * reconfigure by bb_wdt_invert_core_mask(); the result is converted back to
 * ESP-IDF's monitored polarity, again exactly once, immediately before
 * building esp_task_wdt_config_t. claim/release each trigger their own
 * reconfigure, so there is no observable "declared but not applied" window.
 *
 * Exclusivity of OWNERSHIP (as opposed to idle-check suppression) is
 * enforced ATOMICALLY by bb_wdt_claim_core_exclusive() below (B1-1364
 * PR3-fix): it checks-and-sets the claimed-core bit under bb_wdt's own
 * lock, so two concurrent core_owning==true bb_task_create() calls
 * targeting the same core can never both succeed -- exactly one gets
 * BB_OK, the other BB_ERR_INVALID_STATE. bb_task_resolve() (B1-1364 PR3)
 * ALSO rejects a cfg->core_owning == true request targeting a core already
 * present in the `claimed_core_mask` snapshot passed to it, with
 * BB_ERR_INVALID_ARG -- but that snapshot is read by the caller (the
 * platform creation shell) before bb_task_resolve() runs and can go stale
 * before the shell reaches the real claim call, so this is a FAST PATH /
 * early error only (avoids the cost of resolving + attempting creation for
 * the common, non-racing case), never the enforcement mechanism itself.
 * The platform creation shells call bb_wdt_claim_core_exclusive() (never
 * plain bb_wdt_claim_core()) for a core_owning==true request precisely so
 * the real exclusivity guarantee holds even when two creations race.
 * bb_wdt_claim_core() itself stays permissive (accumulate, not reject) for
 * non-exclusive callers -- it has no notion of "owner" at all, only
 * "claimed" -- callers that bypass bb_task_create() and call
 * bb_wdt_claim_core() directly are NOT protected by either check.
 *
 * Ordering requirement: a core-owning (pinned) task must be CREATED --
 * i.e. bb_wdt_claim_core() must have already been called -- BEFORE any
 * unaffinitized worker-spawning component starts, or bb_wdt_steer_core()
 * has nothing to steer away from and the worker may land on the claimed
 * core.
 *
 * Single-core targets (esp32-s2, esp32-c3): bb_wdt_claim_core(1) still
 * bookkeeps successfully (claiming is not validated against the real core
 * count) and bb_wdt_claimed_core_mask()/bb_wdt_steer_core() see the claim,
 * but it has NO effect on the applied Task WDT idle mask -- the platform
 * reconfigure path runs the derived excused mask through
 * bb_wdt_clamp_core_mask() with the target's real core count before calling
 * esp_task_wdt_reconfigure(), which otherwise rejects (ESP_ERR_INVALID_ARG)
 * any idle_core_mask bit at or above CONFIG_FREERTOS_NUMBER_OF_CORES. This
 * clamp is load-bearing: without it, a single out-of-range claim would make
 * esp_task_wdt_reconfigure() fail on every subsequent reconfigure (timeout
 * changes included) until the offending core was released.
 * bb_wdt_steer_core() with num_cores <= 1 always returns the request
 * unchanged -- there is nothing to steer away from.
 * ---------------------------------------------------------------------------
 */

/* Sentinel "no explicit core" request, mirroring bb_task's BB_TASK_CORE_ANY
 * (kept independent here so bb_wdt does not take on a bb_task dependency
 * before the integration PR). Pass to bb_wdt_steer_core() as `requested`. */
#define BB_WDT_CORE_ANY (-1)

/*
 * ---------------------------------------------------------------------------
 * Polarity hardening (B1-1408).
 *
 * ESP-IDF's own esp_task_wdt.h idle_core_mask is MONITORED polarity: "1 << i
 * means that core i's idle task will be monitored by the TWDT" -- a SET bit
 * means the core IS watched, not that it is excused. bb_wdt's entire
 * internal representation is instead EXCUSED polarity: a SET bit means that
 * core's idle check is SUPPRESSED (the opposite meaning). The two masks are
 * NOT interchangeable -- OR-ing an excused-polarity claim into a
 * monitored-polarity mask (the historical bug this fixes) makes a claimed
 * core MORE monitored, not less. These are wrapped single-member structs
 * (not bare uint32_t typedefs) specifically so the compiler rejects an
 * implicit interchange between the two polarities -- the exact class of
 * mistake that let the bug ship and survive a review cycle.
 *
 * bb_wdt_derive_excused_mask() and bb_wdt_clamp_core_mask() speak ONLY
 * excused polarity -- ENFORCED, not just documented: both of
 * bb_wdt_derive_excused_mask()'s parameters are bb_wdt_excused_mask_t (a
 * claim IS excused-polarity by contract -- claiming a core means "suppress
 * its idle check", the same meaning as a set bit in this type), so the
 * compiler rejects passing a bare/monitored-polarity uint32_t into either
 * slot. bb_wdt_claimed_core_mask() itself stays a bare uint32_t (it is also
 * consumed as a raw ownership bitmask outside bb_wdt, by bb_task_resolve()
 * and bb_http_server's worker steering); its two bb_wdt_derive_excused_mask()
 * call sites (both platform do_reconfigure() paths) each wrap it in a single,
 * explicitly named bb_wdt_excused_mask_t conversion at the call site -- never
 * an implicit/blind cast. The ONLY place bb_wdt may hold or write a
 * monitored-polarity value is the platform reconfigure path's final
 * conversion, immediately before it is assigned to
 * esp_task_wdt_config_t.idle_core_mask -- see bb_wdt_invert_core_mask().
 * ---------------------------------------------------------------------------
 */
typedef struct { uint32_t bits; } bb_wdt_excused_mask_t;
typedef struct { uint32_t bits; } bb_wdt_monitored_mask_t;

/*
 * Claim `core` (0 or 1): the Task WDT idle check for that core will be
 * excused on every subsequent reconfigure. Idempotent -- claiming an
 * already-claimed core (same or different caller) is a no-op success, not
 * an error. Triggers an immediate reconfigure with the updated mask.
 * Returns BB_OK, or BB_ERR_INVALID_ARG if core is not 0 or 1.
 */
bb_err_t bb_wdt_claim_core(int core);

/*
 * Atomically claim `core` (0 or 1) EXCLUSIVELY: under bb_wdt's own lock,
 * checks whether `core` is already claimed and, only if not, sets the bit
 * and returns BB_OK -- all as a single critical section, so two concurrent
 * callers targeting the same core can never both succeed. Returns
 * BB_ERR_INVALID_STATE if `core` is already claimed (by this or any other
 * caller), or BB_ERR_INVALID_ARG if core is not 0 or 1. Triggers an
 * immediate reconfigure with the updated mask on success only.
 *
 * This is the primitive an exclusive OWNER (as opposed to a plain
 * idle-check-suppression claim via bb_wdt_claim_core()) must call --
 * bb_task_create()'s creation shells use this for cfg->core_owning == true
 * requests. See the "Exclusivity of OWNERSHIP" note above for why the
 * resolve-time bb_task_resolve() check alone is not sufficient.
 */
bb_err_t bb_wdt_claim_core_exclusive(int core);

/*
 * Release a previous claim on `core` (0 or 1). Tolerant: returns BB_OK if
 * `core` was never claimed. Triggers an immediate reconfigure with the
 * updated mask. Returns BB_ERR_INVALID_ARG if core is not 0 or 1.
 */
bb_err_t bb_wdt_release_core(int core);

/*
 * Current claimed-core bitmask (bit N set iff core N is claimed). 0 if
 * nothing is claimed.
 */
uint32_t bb_wdt_claimed_core_mask(void);

/*
 * PURE: derive the excused-core mask actually applied to the Task WDT --
 * always the union (bitwise OR) of `excused_seed` (the board's
 * Kconfig-configured default, already converted to EXCUSED polarity -- see
 * "Polarity hardening" above and bb_wdt_invert_core_mask()) and
 * `claimed_mask` (a claimed-core mask -- see bb_wdt_claimed_core_mask() --
 * wrapped as bb_wdt_excused_mask_t at the call site, since a claim IS
 * excused polarity by contract). Never overrides or removes a bit either
 * side sets; a claim only ever adds an excused core. Both parameters are
 * typed bb_wdt_excused_mask_t specifically so this can never accept a raw
 * ESP-IDF monitored-polarity mask (e.g. the un-inverted output of the
 * platform's Kconfig read), which would reproduce the exact defect this type
 * split exists to prevent (B1-1408). Exposed for host testing; the platform
 * reconfigure path is the only real caller.
 */
bb_wdt_excused_mask_t bb_wdt_derive_excused_mask(bb_wdt_excused_mask_t excused_seed,
                                                  bb_wdt_excused_mask_t claimed_mask);

/*
 * PURE: clamp a derived excused-core mask to the cores that actually exist
 * on this target (num_cores). This is pure range-truncation math and is
 * agnostic to polarity in isolation, but it is only ever called on an
 * EXCUSED-polarity mask in this codebase (see "Polarity hardening" above):
 * esp_task_wdt_reconfigure() returns ESP_ERR_INVALID_ARG for any
 * idle_core_mask with a bit set at or above num_cores -- e.g. bit 1 on a
 * 1-core esp32-s2/-c3 build (CONFIG_FREERTOS_NUMBER_OF_CORES == 1). Every
 * platform reconfigure call MUST clamp through this before calling
 * esp_task_wdt_reconfigure(), so a claim on a core that doesn't exist on
 * this target can never produce a mask the real API rejects (which would
 * otherwise silently drop every subsequent reconfigure, including unrelated
 * timeout changes, until the offending claim is released -- see
 * bb_wdt_claim_core()'s unicore note).
 *
 * num_cores <= 0 clamps to 0 (defensive; not a real target configuration).
 * num_cores >= 32 returns mask unchanged (no bit can be out of range for a
 * uint32_t mask). Exposed for host testing; the platform reconfigure path
 * is the only real caller.
 */
bb_wdt_excused_mask_t bb_wdt_clamp_core_mask(bb_wdt_excused_mask_t mask, int num_cores);

/*
 * PURE, self-inverse: flip a core mask's polarity within the cores that
 * actually exist on this target -- `~mask & full_mask(num_cores)`, where
 * full_mask(num_cores) has exactly the low `num_cores` bits set. Applying it
 * twice returns the original value, provided that value has no bit set
 * outside num_cores (e.g. already clamped via bb_wdt_clamp_core_mask()).
 *
 * This is the ONE symmetric helper that crosses BOTH polarity boundaries
 * bb_wdt has (see "Polarity hardening" above):
 *   (1) the platform's Kconfig-derived idle-check default is computed in
 *       ESP-IDF's own MONITORED polarity (mirroring ESP-IDF's own
 *       app_startup.c computation from the same Kconfig symbols);
 *       bb_wdt_invert_core_mask() converts it ONCE into the EXCUSED
 *       polarity bb_wdt_derive_excused_mask() expects as `excused_seed`.
 *   (2) the platform reconfigure path converts the final derived + clamped
 *       EXCUSED mask back into MONITORED polarity, again EXACTLY ONCE,
 *       immediately before writing esp_task_wdt_config_t.idle_core_mask --
 *       the only place in bb_wdt that may hold a monitored-polarity value.
 *
 * Deliberately typed as bare uint32_t in and out, not either
 * bb_wdt_excused_mask_t or bb_wdt_monitored_mask_t: it IS the conversion
 * between the two, so it straddles both types by definition -- wrapping it
 * to one side would just force an unsafe cast at its own two call sites.
 * Exposed for host testing; the platform reconfigure path (both boundaries)
 * is the only real caller.
 */
uint32_t bb_wdt_invert_core_mask(uint32_t mask, int num_cores);

/*
 * PURE: steer a requested core affinity away from a core claimed by
 * another task, so an unaffinitized worker doesn't land on (and starve)
 * the core a pinned task already owns.
 *
 * - requested == 0 or 1 (an explicit pin): passed through UNCHANGED. An
 *   explicit pin always wins over steering.
 * - requested == BB_WDT_CORE_ANY: if EXACTLY ONE of core 0/1 is claimed in
 *   claimed_mask, returns the OTHER core; if zero or both are claimed,
 *   returns BB_WDT_CORE_ANY unchanged (nothing to steer toward).
 * - num_cores < 2 (unicore target): always returns `requested` unchanged --
 *   there is only one core, so there is nothing to steer away from.
 *
 * num_cores is a parameter (not a compile-time constant) for host
 * testability and single-core correctness.
 */
int bb_wdt_steer_core(int requested, uint32_t claimed_mask, int num_cores);

#ifdef __cplusplus
}
#endif
