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
 * Subscribe the calling task to the Task WDT (esp_task_wdt_add(NULL)).
 * Returns BB_OK on success, or a platform error code on failure.
 * No-op / returns BB_OK on host.
 *
 * Thin wrapper over bb_wdt_task_subscribe_handle(NULL).
 */
bb_err_t bb_wdt_task_subscribe(void);

/*
 * Unsubscribe the calling task from the Task WDT (esp_task_wdt_delete(NULL)).
 * Returns BB_OK on success, BB_OK if not subscribed, or a platform error.
 * No-op / returns BB_OK on host.
 *
 * Thin wrapper over bb_wdt_task_unsubscribe_handle(NULL).
 */
bb_err_t bb_wdt_task_unsubscribe(void);

/*
 * Subscribe an arbitrary task handle to the Task WDT (esp_task_wdt_add(handle)).
 * `handle` is an opaque platform task handle (TaskHandle_t on ESP-IDF); pass
 * NULL to subscribe the calling task, equivalent to bb_wdt_task_subscribe().
 * Single wrapper over esp_task_wdt_add — callers must not call the raw
 * ESP-IDF API directly.
 * Returns BB_OK on success, or a platform error code on failure.
 * No-op / returns BB_OK on host.
 */
bb_err_t bb_wdt_task_subscribe_handle(void *handle);

/*
 * Unsubscribe an arbitrary task handle from the Task WDT
 * (esp_task_wdt_delete(handle)). `handle` is an opaque platform task handle
 * (TaskHandle_t on ESP-IDF); pass NULL to unsubscribe the calling task,
 * equivalent to bb_wdt_task_unsubscribe(). Single wrapper over
 * esp_task_wdt_delete — callers must not call the raw ESP-IDF API directly.
 * Returns BB_OK on success, BB_OK if not subscribed, or a platform error.
 * No-op / returns BB_OK on host.
 */
bb_err_t bb_wdt_task_unsubscribe_handle(void *handle);

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
 * bb_wdt_release_core()) computes `kconfig_default_mask | claimed_mask` --
 * the claim only ever ADDS an excused core, never removes a check the board
 * owner asked for via Kconfig on an unclaimed core. claim/release each
 * trigger their own reconfigure, so there is no observable
 * "declared but not applied" window.
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
 * reconfigure path runs the derived mask through bb_wdt_clamp_idle_mask()
 * with the target's real core count before calling
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
 * PURE: derive the idle-check mask actually applied to the Task WDT --
 * always the union (bitwise OR) of the board's Kconfig-configured default
 * mask and the currently claimed-core mask. Never overrides or removes a
 * bit either side sets; a claim only ever adds an excused core. Exposed for
 * host testing; the platform reconfigure path is the only real caller.
 */
uint32_t bb_wdt_derive_idle_mask(uint32_t kconfig_default_mask, uint32_t claimed_mask);

/*
 * PURE: clamp a derived idle-check mask to the cores that actually exist on
 * this target (num_cores). esp_task_wdt_reconfigure() returns
 * ESP_ERR_INVALID_ARG for any idle_core_mask with a bit set at or above
 * num_cores -- e.g. bit 1 on a 1-core esp32-s2/-c3 build
 * (CONFIG_FREERTOS_NUMBER_OF_CORES == 1). Every platform reconfigure call
 * MUST clamp through this before calling esp_task_wdt_reconfigure(), so a
 * claim on a core that doesn't exist on this target can never produce a
 * mask the real API rejects (which would otherwise silently drop every
 * subsequent reconfigure, including unrelated timeout changes, until the
 * offending claim is released -- see bb_wdt_claim_core()'s unicore note).
 *
 * num_cores <= 0 clamps to 0 (defensive; not a real target configuration).
 * num_cores >= 32 returns mask unchanged (no bit can be out of range for a
 * uint32_t mask). Exposed for host testing; the platform reconfigure path
 * is the only real caller.
 */
uint32_t bb_wdt_clamp_idle_mask(uint32_t mask, int num_cores);

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
