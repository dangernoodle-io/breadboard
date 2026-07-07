#pragma once
// bb_task — SSOT task-creation primitive (task-registry unification, PR2b).
//
// Collapses the 2x2 xTaskCreate variant matrix (dynamic/static backing x
// plain/pinned-to-core affinity) behind a single config-struct call, and
// owns the two axes every call site otherwise hand-rolls: the unicore
// core-affinity clamp and the stack bytes->words conversion.
//
// A pointer-keyed base registry (bb_registry, keyed by the opaque task
// handle) records one entry per task created through bb_task_create() --
// name, stack budget, the wdt_arm data flag, and a mark-and-sweep
// `seen_tick` slot for a later periodic reconciliation pass.
//
// DORMANT IN THIS PR: no in-tree consumer calls bb_task_create() yet. A
// later PR (task-registry unification PR3) migrates existing xTaskCreate*
// call sites onto this API and wires the periodic sweep scan that drives
// bb_task_base_sweep_apply(). This PR ships the primitive + pure evaluator
// + host tests only.
//
// Portable: no FreeRTOS types appear in this header. `bb_task_entry_fn_t`
// matches FreeRTOS's TaskFunction_t signature (`void (*)(void *)`) without
// requiring a FreeRTOS include; task handles and the STATIC backing's
// stack/TCB buffers are carried as opaque `void *` (TaskHandle_t is itself
// an opaque `void *` on FreeRTOS, and StackType_t*/StaticTask_t* are only
// dereferenced inside the espidf platform shell) -- see CLAUDE.md "Header
// visibility and component coupling".

#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Kconfig bridge (see CLAUDE.md "Avoiding audit-class regressions"). This is
// the ONE real symbol for bb_task's base-registry capacity -- bb_task_common.c
// and every other consumer that must size a buffer off this same capacity
// (e.g. components/bb_task_registry/bb_task_registry_base_scan_common.c's
// reconcile buffer) reference BB_TASK_BASE_MAX_CAP rather than re-deriving
// their own copy of CONFIG_BB_TASK_BASE_MAX, so the two can never silently
// diverge.
#ifdef ESP_PLATFORM
#  include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_TASK_BASE_MAX
#  define BB_TASK_BASE_MAX_CAP CONFIG_BB_TASK_BASE_MAX
#endif
#ifndef BB_TASK_BASE_MAX_CAP
#define BB_TASK_BASE_MAX_CAP 24
#endif

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// Matches FreeRTOS's TaskFunction_t signature (`void (*)(void *)`).
typedef void (*bb_task_entry_fn_t)(void *arg);

// Core affinity. BB_TASK_CORE_ANY maps to tskNO_AFFINITY in the espidf
// shell (plain xTaskCreate[Static]); 0/1 request xTaskCreate[Static]
// PinnedToCore, clamped to BB_TASK_CORE_ANY by bb_task_resolve() when the
// target has fewer cores than requested (unicore targets).
#define BB_TASK_CORE_ANY (-1)

typedef enum {
    BB_TASK_BACKING_DYNAMIC = 0,  // xTaskCreate[PinnedToCore]
    BB_TASK_BACKING_STATIC  = 1,  // xTaskCreateStatic[PinnedToCore]
} bb_task_backing_t;

// Task-creation config. Designated-initializer only -- no positional
// construction is supported, so new fields are additive/non-breaking.
typedef struct {
    bb_task_entry_fn_t entry;        // required
    const char        *name;         // task name; required
    void              *arg;          // passed through to entry
    uint32_t           stack_bytes;  // converted to words once, inside bb_task
    uint32_t           priority;     // UBaseType_t on the espidf side
    int                core;         // BB_TASK_CORE_ANY / 0 / 1

    bb_task_backing_t  backing;
    // STATIC only -- opaque StackType_t*/StaticTask_t* buffers, required
    // when backing == BB_TASK_BACKING_STATIC (validated by bb_task_resolve).
    void              *stack_buf;
    void              *tcb_buf;

    // Data flag only -- recorded on the base registry entry for later
    // diagnostic surfacing. bb_task does NOT arm any watchdog itself (no
    // bb_wdt dependency here); a later consumer may act on this flag.
    bool                wdt_arm;
} bb_task_config_t;

// Resolved/validated result of bb_task_resolve() -- what the espidf shell
// hands to the matching xTaskCreate* variant.
typedef struct {
    int                core;         // post-clamp
    uint32_t           stack_bytes;  // passthrough of cfg->stack_bytes -- the
                                      // portable resolver does not know the
                                      // platform's StackType_t size; the
                                      // bytes -> xTaskCreate* depth
                                      // conversion (/ sizeof(StackType_t))
                                      // happens in the espidf shell.
    bb_task_backing_t  backing;
} bb_task_resolved_t;

// Pure resolver -- no FreeRTOS types, no task creation. Validates `cfg` and
// fills `*out` on success.
// `num_cores` is the caller-supplied core count (configNUMBER_OF_CORES on
// the espidf shell; any value on host tests) so the unicore clamp stays
// host-testable without a FreeRTOS dependency.
// Returns BB_ERR_INVALID_ARG when cfg or out is NULL, cfg->entry is NULL,
// cfg->stack_bytes is 0, or cfg->backing == BB_TASK_BACKING_STATIC and
// cfg->stack_buf/cfg->tcb_buf is NULL.
bb_err_t bb_task_resolve(const bb_task_config_t *cfg, int num_cores,
                          bb_task_resolved_t *out);

// ---------------------------------------------------------------------------
// Base registry -- pointer-keyed (opaque task handle, TaskHandle_t on
// ESP-IDF), one entry per task created via bb_task_create(). Sized by
// CONFIG_BB_TASK_BASE_MAX (bridged to BB_TASK_BASE_MAX, C default 24).
// ---------------------------------------------------------------------------

#define BB_TASK_NAME_MAX 20

typedef struct {
    bool      in_use;
    void     *handle;
    char      name[BB_TASK_NAME_MAX];
    uint32_t  stack_bytes;
    bool      wdt_arm;
    uint32_t  seen_tick;
} bb_task_base_entry_t;

// Create-if-absent, update-if-present. Tolerates re-invocation on the same
// handle (pool-recycle reuse, e.g. sse_N-style task-slot churn) -- never
// double-inserts.
// Returns BB_ERR_INVALID_ARG if handle or name is NULL.
// Returns BB_ERR_NO_SPACE if the base registry is full and handle is new.
bb_err_t bb_task_base_upsert(void *handle, const char *name,
                              uint32_t stack_bytes, bool wdt_arm);

// Remove the base entry for `handle`.
// Returns BB_ERR_INVALID_ARG if handle is NULL.
// Returns BB_ERR_NOT_FOUND if handle was never upserted.
bb_err_t bb_task_base_remove(void *handle);

// Tag `handle`'s base entry as seen this scan (updates seen_tick only --
// never touches name/stack_bytes/wdt_arm).
// Returns BB_ERR_INVALID_ARG if handle is NULL.
// Returns BB_ERR_NOT_FOUND if handle was never upserted.
bb_err_t bb_task_base_touch(void *handle, uint32_t now_tick);

// Atomic touch-or-insert -- a SINGLE critical section performs the
// lookup-then-touch-or-insert decision, closing the race a separately
// locked touch() + upsert() pair would otherwise expose: if a concurrent
// bb_task_create()/bb_task_registry_register() inserts the REAL entry
// (real stack_bytes/wdt_arm) for `handle` in the gap between an unlocked
// "not found" and a fallback upsert, the fallback's update-if-present
// branch would clobber the real values with the placeholder (0, false).
// Used by the periodic base-scan consumer (task-registry unification PR3)
// in place of bb_task_base_touch() + a conditional bb_task_base_upsert().
//
// If `handle` is already tracked: bumps seen_tick only (mirrors
// bb_task_base_touch() -- never overwrites name/stack_bytes/wdt_arm, which
// are owned by bb_task_create()/bb_task_registry_register()'s
// overlay-join).
// If `handle` is not yet tracked: inserts a best-effort placeholder
// (`name`, stack_bytes=0, wdt_arm=false, seen_tick=now_tick).
// Returns BB_ERR_INVALID_ARG if handle or name is NULL.
// Returns BB_ERR_NO_SPACE if the base registry is full and handle is new.
bb_err_t bb_task_base_touch_or_insert(void *handle, const char *name, uint32_t now_tick);

typedef void (*bb_task_base_cb_t)(void *handle, const bb_task_base_entry_t *entry, void *ctx);

// Iterate every base entry (snapshot-then-notify semantics, via
// bb_registry_foreach_ptr). Safe to call from multiple threads; do not call
// bb_task_base_upsert/_remove from within cb (same stale-snapshot caveat as
// bb_registry_foreach_ptr).
void bb_task_base_foreach(bb_task_base_cb_t cb, void *ctx);

// Occupancy accessors (B1-601 re-scope) -- mirror bb_task_registry_count()/
// _capacity()/_dropped()'s exact signatures/types for drop-in parity. This
// base registry is the SSOT for /api/diag/tasks' registry.{count,capacity,
// dropped} aggregate: it owns the fixed task table every bb_task_create()
// call hits, so overflow observed here reflects the real task-creation
// pool, unlike bb_task_registry (which only counts health-registered
// tasks).

// Count of in_use base-registry entries, read under lock.
uint16_t bb_task_base_count(void);

// Fixed pool capacity (BB_TASK_BASE_MAX_CAP).
uint16_t bb_task_base_capacity(void);

// Monotonic count of bb_task_base_upsert()/_touch_or_insert() calls
// rejected with BB_ERR_NO_SPACE because the pool was full.
uint32_t bb_task_base_dropped(void);

// Pure mark-and-sweep evaluator over a caller-supplied entries array -- NO
// FreeRTOS types, no access to the internal base registry above. An entry
// with `seen_tick == now_tick` is treated as seen (alive) this scan and
// left untouched. Grace window: an entry survives exactly one missed scan
// (tolerates a transient uxTaskGetSystemState omission -- same rationale as
// bb_health_stack's mark/sweep table); it is freed (in_use cleared, entry
// zeroed) only once it has been missed for MORE than one consecutive scan.
// Returns the count of entries freed.
//
// This is the pure decision function only -- the periodic scan that marks
// entries (by re-upserting seen_tick) and reconciles freed entries back
// into the live base registry (via bb_task_base_remove) is wired in a
// later PR, not here.
int bb_task_base_sweep_apply(bb_task_base_entry_t *entries, int n, uint32_t now_tick);

// ---------------------------------------------------------------------------
// Create/deregister -- the SSOT task-creation primitive. DORMANT in this
// PR: no in-tree caller yet.
// ---------------------------------------------------------------------------

// Resolve + validate cfg, create the task via the matching xTaskCreate*
// variant, then upsert a base registry entry. `out_handle` may be NULL;
// when non-NULL it is always set (to NULL on failure, to the new task
// handle on success).
// Returns whatever bb_task_resolve() would return on a validation failure.
// Returns BB_ERR_NO_MEM if task creation itself fails.
bb_err_t bb_task_create(const bb_task_config_t *cfg, void **out_handle);

// Remove the base registry entry for a task created via bb_task_create().
// Does NOT delete the FreeRTOS task itself -- callers still own their own
// vTaskDelete/self-delete lifecycle (mirrors bb_task_registry_deregister).
// Returns BB_ERR_INVALID_ARG if handle is NULL, BB_ERR_NOT_FOUND if handle
// was never registered.
bb_err_t bb_task_deregister(void *handle);

#ifdef BB_TASK_TESTING
// Reset the base registry to its initial (empty) state. Test teardown only.
void bb_task_base_test_reset(void);

// Test-only race injection: arms a one-shot hook that fires INSIDE
// bb_task_base_touch_or_insert()'s critical section, immediately after it
// determines `handle` is not yet tracked and before it inserts its own
// placeholder. The hook inserts the "real" entry described here (as
// bb_task_create()/bb_task_registry_register() would) directly into the
// pool/registry, then touch_or_insert() re-checks presence before falling
// through to its own insert -- proving the atomic path tolerates a
// same-tick competing insert instead of assuming its earlier "not found"
// snapshot is still valid. Consumed at most once; a no-op if never armed.
void bb_task_base_test_arm_race_insert(void *handle, const char *name,
                                        uint32_t stack_bytes, bool wdt_arm);
#endif

#ifdef __cplusplus
}
#endif
