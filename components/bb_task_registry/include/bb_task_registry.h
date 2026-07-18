#pragma once

// bb_task_registry — thin consumer of the generic bb_registry primitive that
// tracks every self-registered FreeRTOS task under a diagnostic name, for
// surfacing at GET /api/diag/tasks (bb_diag).
//
// PLUMBING (B1-458 PR-A). This component owns per-task hardware Task-WDT
// subscribe/feed wiring via `bb_task_registry_opts_t` + a generation-checked
// token.
//
// SOFTWARE WATCHDOG MONITOR (B1-458 PR-B). Built on top of PR-A's
// self-feed/token plumbing: `opts->sw_wdt_timeout_ms` (0 = off) arms a
// per-task software miss check, evaluated by `bb_task_registry_sw_wdt_check()`
// — a pure, host-testable evaluator driven by a periodic monitor task
// (`platform/espidf/bb_task_registry/bb_task_registry_monitor.c`, gated by
// `CONFIG_BB_TASK_REGISTRY_SW_WDT`) on ESP-IDF. Miss episodes are reported
// via an optional handler (`bb_task_registry_set_sw_wdt_handler`) and
// surfaced diagnostically via `bb_task_registry_lookup_sw_wdt()`. This is
// OBSERVE-ONLY — it never reboots or otherwise acts on a miss.
//
// Call bb_task_registry_register() once, immediately after a successful
// xTaskCreate*() at each task-creation site, passing the stack budget already
// known at that call site (bytes) and the resulting task handle (opaque
// void* — TaskHandle_t on ESP-IDF). Best-effort: a full registry
// (BB_TASK_REGISTRY_MAX) or a duplicate name is logged and does NOT fail
// task creation. `handle` may be NULL when the site does not retain one.
// Pass `opts` to request hardware Task-WDT subscription (NULL == no
// subscription, matching legacy behavior).
//
// Call bb_task_registry_deregister() before a task's own vTaskDelete(NULL)
// if the task self-deletes (e.g. a one-shot worker) so the registry does not
// retain a stale entry for a task that no longer exists.
//
// This header contains no FreeRTOS types — the task handle is carried as an
// opaque void* — keeping it portable per project convention.

#include <stdint.h>
#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BB_TASK_REGISTRY_NAME_MAX 20

// Per-registration options.
typedef struct {
    bool hw_wdt_subscribe;       // true: register() subscribes handle via bb_wdt_task_subscribe_handle()
    uint32_t sw_wdt_timeout_ms;  // 0 = software watchdog off for this task
} bb_task_registry_opts_t;

// Opaque handle to a live registration, returned by register() and consumed
// by bb_task_registry_feed(). `generation` invalidates a token once the slot
// backing `index` has been deregistered and (possibly) reused by a later
// registration.
// `generation` is uint16_t under the assumption that registrations are
// boot-once / long-lived infra tasks (the registry does not see high-churn
// slot reuse). A future consumer with high-churn slot reuse (many
// register/deregister cycles on the same slot within a boot) would need to
// widen this to uint32_t to avoid wraparound aliasing an old token.
typedef struct {
    uint16_t index;
    uint16_t generation;
} bb_task_registry_token_t;

#define BB_TASK_REGISTRY_TOKEN_INVALID ((bb_task_registry_token_t){ .index = UINT16_MAX, .generation = 0 })

// Register a task under `name`, recording its stack budget (bytes) and
// opaque handle. Best-effort — see file header.
// `opts` may be NULL (equivalent to `{.hw_wdt_subscribe = false}`). When
// `opts->hw_wdt_subscribe` is true and `handle` is non-NULL, register()
// makes a best-effort attempt to subscribe `handle` to the hardware Task
// WDT — a failure is logged but never fails registration.
// `out_token` may be NULL; when non-NULL it receives a token usable with
// bb_task_registry_feed() for the lifetime of this registration.
// Returns BB_ERR_INVALID_ARG if name is NULL.
// Returns BB_ERR_NO_SPACE if the registry is full.
// Returns BB_ERR_INVALID_STATE on a duplicate name.
// `name` must be <= configMAX_TASK_NAME_LEN (typically 16) to match the
// kernel-truncated name reported by the /api/diag/tasks lookup; longer names
// still register but will not correlate against a live TaskStatus_t entry.
bb_err_t bb_task_registry_register(const char *name, uint32_t stack_budget_bytes, void *handle,
                                    const bb_task_registry_opts_t *opts,
                                    bb_task_registry_token_t *out_token);

// Deregister a previously-registered task by handle value. Resolves the
// registered name internally (scans the registry for a matching handle) so
// callers do not need to track the name separately.
// Returns BB_ERR_INVALID_ARG if handle is NULL.
// Returns BB_ERR_NOT_FOUND if handle was never registered (e.g. registration
// failed at creation time due to a full registry, or handle was NULL at
// registration time).
//
// Note: a stale entry left behind by a task that never called deregister
// before self-deleting could, in principle, be matched first if FreeRTOS
// later reuses the freed TaskHandle_t for a new task. This is acceptable
// today because every self-deleting task in-tree calls deregister
// immediately before vTaskDelete(NULL), so no stale entry with a live
// (reusable) handle should exist in practice.
bb_err_t bb_task_registry_deregister(void *handle);

// Current registered task count.
uint16_t bb_task_registry_count(void);

// Registry capacity (BB_TASK_REGISTRY_MAX, bridged from CONFIG_BB_TASK_REGISTRY_MAX).
// Constant for the life of the process — no lock needed.
uint16_t bb_task_registry_capacity(void);

// Monotonic count of register() calls rejected because the pool was full
// (BB_ERR_NO_SPACE). Never resets except via bb_task_registry_test_reset().
// Surfaced at GET /api/diag/tasks (bb_diag) as "registry.dropped" so an
// overflowing registry is observable on-device instead of only a one-line
// bb_log_w at the moment of the drop (B1-471).
uint32_t bb_task_registry_dropped(void);

// Pure lookup — the host-testable seam consumed by the /api/diag/tasks
// handler (bb_diag). Returns true and fills *out_budget (if non-NULL) and *out_wdt (if
// non-NULL) when `name` is registered; returns false otherwise (out params
// left untouched).
bool bb_task_registry_lookup_budget(const char *name, uint32_t *out_budget, bool *out_wdt);

// Iterate all registered tasks. Holds the internal lock across the ENTIRE
// call, including every invocation of cb — mirrors bb_queue_registry_foreach's
// contract (see bb_queue_registry.h for the full rationale: this prevents a
// concurrent deregister from invalidating an entry mid-read).
//
// Foreach contract — cb MUST be:
//   - bounded (finite, fast — it runs while the registry is fully locked)
//   - allocation-free
//   - a pure snapshot/copy-out of name/stack_budget_bytes/wdt_subscribed
//   - MUST NOT perform I/O of any kind
//   - non-blocking otherwise too (no other locks, no waiting)
//   - MUST NOT call back into any bb_task_registry_* function (register,
//     deregister, foreach, count, lookup_budget, test_reset) — the lock is
//     not recursive and doing so self-deadlocks.
typedef void (*bb_task_registry_cb_t)(const char *name, uint32_t stack_budget_bytes,
                                       bool wdt_subscribed, void *ctx);
void bb_task_registry_foreach(bb_task_registry_cb_t cb, void *ctx);

// Feed the hardware Task WDT for the registration identified by `token`.
// LOCK-FREE hot path — safe to call at any rate, including from inside a
// tight loop; MUST NOT take s_task_reg_lock.
//
// SELF-FEED ONLY. This MUST be called by the task that owns `token` (i.e.
// the task that received `token` from its own bb_task_registry_register()
// call). It is NOT a mechanism for feeding the hardware WDT on behalf of
// another task — the underlying esp_task_wdt_reset() pass-through always
// resets the CALLING task's Task-WDT entry, regardless of which slot
// `token` refers to. `token` is merely an O(1) self-identifier into the
// caller's own registry entry, not a cross-task feed handle. Calling this
// from a different task than the owner will not feed the owner's hardware
// WDT entry (a warning is logged on ESP-IDF; last_feed is still recorded).
//
// If `token` is stale (the slot has since been deregistered and possibly
// reused by a different registration — detected via generation mismatch) or
// out of range (e.g. BB_TASK_REGISTRY_TOKEN_INVALID), this is a silent
// no-op. If the registration behind `token` was not subscribed to the
// hardware WDT (opts was NULL or hw_wdt_subscribe was false), the WDT feed
// is skipped, but the token's last-feed timestamp is still advanced — this
// is intentional plumbing for a later software-watchdog monitor PR.
void bb_task_registry_feed(bb_task_registry_token_t token);

// --- Software watchdog monitor (B1-458 PR-B) ---------------------------

// Miss handler, invoked once per miss EPISODE (debounced — not once per
// check tick) from bb_task_registry_sw_wdt_check(), OUTSIDE the internal
// lock. `name` and `handle` identify the offending task; `overrun_ms` is how
// far past sw_wdt_timeout_ms the last feed is. Observe-only: this API never
// reboots or otherwise acts on a miss — that policy is entirely the
// consumer's.
typedef void (*bb_task_registry_sw_wdt_handler_t)(const char *name, void *handle,
                                                   uint32_t overrun_ms, void *ctx);

// Register a miss handler (NULL to clear). Call once at startup, before the
// monitor task (or bb_task_registry_sw_wdt_check()) starts running — not
// synchronized against concurrent check() calls.
void bb_task_registry_set_sw_wdt_handler(bb_task_registry_sw_wdt_handler_t fn, void *ctx);

// Pure per-tick evaluator for the software watchdog — the host-testable seam
// driving both the real monitor task (platform/espidf/bb_task_registry/
// bb_task_registry_monitor.c) and host tests synchronously.
//
// 3-phase, snapshot-first (see bb_ring_diag.c for the general pattern this
// mirrors): (1) copy every in-use entry with sw_wdt_timeout_ms > 0 into a
// bounded stack snapshot under the internal lock, then unlock; (2) with NO
// lock held, evaluate each snapshot entry for a wrap-safe overdue condition
// `(uint32_t)(now_ms - last_feed_ms) > sw_wdt_timeout_ms`, firing the miss
// handler (if any) at most once per miss episode (debounced via
// miss_active) and staging the resulting miss_count/last_miss_ms/miss_active
// update; (3) re-take the lock and commit each staged update ONLY if the
// slot's current generation still matches the snapshot's generation
// (otherwise the slot was deregistered/reused between phases 1 and 3 — skip
// it). The handler callback always runs outside the lock.
void bb_task_registry_sw_wdt_check(uint32_t now_ms);

// Pure lookup for sw-watchdog diagnostics, mirrors lookup_budget. Returns
// true and fills out params (any of which may be NULL) only when `name` is
// registered AND its sw_wdt_timeout_ms > 0; returns false otherwise (out
// params left untouched). Ages are computed against caller-supplied `now_ms`
// using wrap-safe subtraction; `*out_last_miss_age_ms` is 0 when no miss has
// ever been recorded (mirrors bb_event_ring's "0 if none yet" convention).
bool bb_task_registry_lookup_sw_wdt(const char *name, uint32_t now_ms,
                                     uint32_t *out_timeout_ms,
                                     uint32_t *out_last_feed_age_ms,
                                     uint32_t *out_last_miss_age_ms,
                                     uint32_t *out_miss_count);

// --- Base-scan low-stack observer (task-registry unification PR3) ------

// Low-stack transition handler, invoked once per transition INTO low stack
// (debounced by task handle -- not once per scan) from the periodic base
// scan (platform/espidf/bb_task_registry/bb_task_registry_base_scan.c).
// `name` is the kernel task name; `handle` and `free_bytes` (current stack
// high-water free bytes) identify the offending task. Mirrors
// bb_task_registry_sw_wdt_handler_t's episode-debounce contract.
typedef void (*bb_task_registry_low_stack_handler_t)(const char *name, void *handle,
                                                       uint32_t free_bytes, void *ctx);

// Register a low-stack handler (NULL to clear) and threshold (free bytes
// below which a task is flagged low). Call once at startup, before the
// periodic base-scan job starts running -- not synchronized against
// concurrent scan passes. Replaces bb_health_stack's own former self-scan
// (task-registry unification PR3): bb_health_stack now registers here
// instead of polling FreeRTOS task state itself.
void bb_task_registry_set_low_stack_handler(bb_task_registry_low_stack_handler_t fn,
                                             uint32_t threshold_bytes, void *ctx);

// Start the periodic base-scan job (platform/espidf/bb_task_registry/
// bb_task_registry_base_scan.c). No-op (returns BB_OK) when
// CONFIG_FREERTOS_USE_TRACE_FACILITY is off.
// bbtool:init tier=pre_http fn=bb_task_registry_base_scan_start
bb_err_t bb_task_registry_base_scan_start(void);

// Start the software watchdog monitor task (platform/espidf/
// bb_task_registry/bb_task_registry_monitor.c). No-op (returns BB_OK) when
// CONFIG_BB_TASK_REGISTRY_SW_WDT is off.
// bbtool:init tier=pre_http fn=bb_task_registry_sw_wdt_start
bb_err_t bb_task_registry_sw_wdt_start(void);

#ifdef BB_TASK_REGISTRY_TESTING
// Reset the registry to its initial (empty) state. Test teardown only.
void bb_task_registry_test_reset(void);

// Seed hook: register `name`/`stack_budget_bytes`/`wdt_subscribed` directly,
// without a real task handle. Host tests have no FreeRTOS TaskHandle_t to
// pass, and wdt_subscribed cannot be queried from a real WDT on host — this
// bypasses both, storing the caller-supplied wdt_subscribed value verbatim.
// `out_token` may be NULL; when non-NULL it receives a token usable with
// bb_task_registry_feed() for feed-count assertions in tests.
bb_err_t bb_task_registry_test_seed(const char *name, uint32_t stack_budget_bytes, bool wdt_subscribed,
                                     bb_task_registry_token_t *out_token);

// Test hook: directly set the last-feed timestamp for `token`'s slot,
// bypassing the real bb_task_registry_feed() (which always stamps
// bb_clock_now_ms()). Lets sw-watchdog tests drive
// bb_task_registry_sw_wdt_check() deterministically without a real clock
// mock. Silent no-op if `token.index` is out of range.
void bb_task_registry_test_set_last_feed_ms(bb_task_registry_token_t token, uint32_t ms);
#endif

#ifdef __cplusplus
}
#endif
