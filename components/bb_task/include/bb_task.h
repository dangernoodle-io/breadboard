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

// Kconfig bridge (see wiki Conventions#audit-class-regressions). This is
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

    // Declares `core` a CLAIMED pin (via bb_wdt_claim_core_exclusive(),
    // B1-1364 PR1/PR3-fix) rather than an advisory affinity hint -- the
    // Task WDT idle check for that core is excused for as long as this task
    // lives, and exactly one owning task per core is enforced. Only
    // meaningful when core != BB_TASK_CORE_ANY; bb_task_resolve() rejects
    // core_owning == true paired with core == BB_TASK_CORE_ANY with
    // BB_ERR_INVALID_ARG (an "owning" task with no core to own is a caller
    // bug, not something to silently downgrade).
    //
    // bb_task_resolve() ALSO rejects core_owning == true when `core` is
    // already claimed in the caller-supplied claimed_core_mask snapshot --
    // but that is a FAST-PATH / early error only, not the real enforcement:
    // the snapshot can go stale between when the shell reads it and when it
    // actually claims. The real, race-proof exclusivity guarantee is
    // enforced by the creation shell's bb_wdt_claim_core_exclusive() call
    // (atomic check-and-set under bb_wdt's own lock, see bb_wdt.h) -- a
    // concurrent core_owning == true request that slips past the resolver's
    // stale-snapshot check still gets rejected there, with BB_ERR_INVALID_
    // STATE propagated out of bb_task_create().
    //
    // On a unicore target, bb_task_resolve()'s existing unicore clamp
    // reduces an explicit core to BB_TASK_CORE_ANY first, so core_owning
    // degrades to a no-op claim there -- never call
    // bb_wdt_claim_core_exclusive() with a core that doesn't exist.
    bool                core_owning;

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

// Pure resolver -- no FreeRTOS types, no task creation, no access to any
// global/static state. Validates `cfg` and fills `*out` on success.
// `num_cores` is the caller-supplied core count (configNUMBER_OF_CORES on
// the espidf shell; any value on host tests) so the unicore clamp stays
// host-testable without a FreeRTOS dependency.
// Returns BB_ERR_INVALID_ARG when cfg or out is NULL, cfg->entry is NULL,
// cfg->stack_bytes is 0, cfg->backing == BB_TASK_BACKING_STATIC and
// cfg->stack_buf/cfg->tcb_buf is NULL, cfg->core_owning is true with
// cfg->core == BB_TASK_CORE_ANY, or cfg->core_owning is true and
// `claimed_core_mask` already has cfg->core's bit set (B1-1364 PR3 --
// rejects a second owning claim on a core another task already owns per the
// caller-supplied snapshot). This snapshot-based check is a FAST PATH /
// early error only -- `claimed_core_mask` is read by the caller before this
// function runs and can go stale by the time the creation shell actually
// claims, so a caller must NOT treat a BB_OK return here as a guarantee
// that the eventual claim will succeed. The real, race-proof exclusivity
// guarantee is bb_wdt_claim_core_exclusive() (atomic check-and-set under
// bb_wdt's own lock, see bb_wdt.h), called by the creation shell after this
// resolver returns -- its BB_ERR_INVALID_STATE is what actually protects a
// concurrent double-claim, and is propagated out of bb_task_create().
//
// core-claim steering (B1-1364 PR2, purity restored B1-1356): when
// cfg->core == BB_TASK_CORE_ANY (which, per the validation above, implies
// cfg->core_owning is false), the requested core is steered away from a
// core another task already claims via bb_wdt's core-claim mechanism
// (bb_wdt_steer_core() over `claimed_core_mask`) before the unicore clamp
// runs. `claimed_core_mask` is bb_wdt's live claimed-core state
// (bb_wdt_claimed_core_mask()), read by the CALLER and passed in here --
// this keeps bb_task_resolve() a mathematically pure function of its
// arguments only (order-independent, no hidden global read), rather than a
// function of bb_wdt's live state. The platform creation shells
// (platform/espidf/bb_task/bb_task_espidf.c, platform/host/bb_task/
// bb_task_host.c) read bb_wdt_claimed_core_mask() and pass it through; this
// function performs no side effect of its own (no claim, no WDT
// reconfigure) -- the actual bb_wdt_claim_core() side effect lives in
// those same platform shells, called only after this resolver returns
// BB_OK.
bb_err_t bb_task_resolve(const bb_task_config_t *cfg, int num_cores,
                          uint32_t claimed_core_mask,
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
    // Resolved core affinity (BB_TASK_CORE_ANY / 0 / 1) and whether this
    // task's cfg->core_owning claimed that core via bb_wdt_claim_core() at
    // creation time (B1-1364 PR2) -- recorded by bb_task_create() via
    // bb_task_base_set_core_owning() below, so bb_task_deregister() can
    // release a live claim without the caller re-supplying its own cfg.
    // Both default to the zero value (core == 0, core_owning == false)
    // until bb_task_create() sets them; a base entry created via the
    // legacy bb_task_base_upsert()/_touch_or_insert() paths (not
    // bb_task_create()) never has core_owning set, so bb_task_deregister()
    // never attempts a release for it.
    int       core;
    bool      core_owning;
    uint32_t  seen_tick;
    // Current stack high-water FREE bytes, last observed by the periodic
    // base-scan (platform/espidf/bb_task_registry/bb_task_registry_base_scan.c)
    // via bb_task_base_set_free_bytes() below. 0 until the first scan
    // observes this handle (e.g. between bb_task_create()/
    // bb_task_registry_register() and the next scan tick). Non-increasing
    // over a task's life -- FreeRTOS's own high-water-mark semantics.
    // NOTE: 0 here is ambiguous between "never scanned" and "genuinely no
    // headroom left" -- consumers MUST check `sampled` below rather than
    // treating a bare 0 as a real reading.
    uint32_t  free_bytes;
    // True once bb_task_base_set_free_bytes() has landed at least one
    // sample for this handle; false from insert (upsert zero-initializes
    // the pool slot -- see bb_task_common.c) until that first write. Lets a
    // consumer (e.g. bb_serialize_console_tasks_report()) distinguish
    // "unmeasured" from "measured and free_bytes happens to be 0" instead
    // of relying on scan/report timer choreography to always win the race.
    bool      sampled;
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

// Records `free_bytes` (current stack high-water free bytes) on `handle`'s
// base entry -- does NOT touch seen_tick/name/stack_bytes/wdt_arm. Written
// by the periodic base-scan (task-registry unification PR3) from the SAME
// uxTaskGetStackHighWaterMark() sample already taken for the low-stack
// check, so this persists an already-computed value rather than sampling
// FreeRTOS a second time. Also sets the entry's `sampled` bit -- the ONLY
// place that bit is ever set -- so a consumer reading `free_bytes` before
// this has ever run for `handle` can tell "unmeasured" apart from a
// genuine (and possibly alarming) low reading.
// Returns BB_ERR_INVALID_ARG if handle is NULL.
// Returns BB_ERR_NOT_FOUND if handle was never upserted.
bb_err_t bb_task_base_set_free_bytes(void *handle, uint32_t free_bytes);

// Records the resolved core affinity and core-owning flag for `handle`'s
// base entry (B1-1364 PR2) -- written by bb_task_create() only, immediately
// after upsert, so bb_task_deregister() can release a live bb_wdt core
// claim without the caller re-supplying its own cfg. Does NOT touch
// name/stack_bytes/wdt_arm/seen_tick/free_bytes/sampled.
// Returns BB_ERR_INVALID_ARG if handle is NULL.
// Returns BB_ERR_NOT_FOUND if handle was never upserted.
bb_err_t bb_task_base_set_core_owning(void *handle, int core, bool core_owning);

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

// ---------------------------------------------------------------------------
// Delay / yield -- the canonical replacement for hand-rolled
// vTaskDelay(pdMS_TO_TICKS(N)) call sites (B1-1350, migrated B1-1352). Every
// platform/espidf/ call site that blocks for a millisecond duration goes
// through bb_task_delay_ms()/bb_task_yield() now, EXCEPT: bb_task's own
// espidf shell (bb_task_delay_ms() itself calls vTaskDelay()) and a single
// vTaskDelay(portMAX_DELAY) infinite-block site (bb_ota_boot.c) -- not a
// millisecond delay, so there is no bb_task_delay_ms() equivalent.
//
// components/bb_core/include/bb_once.h's two raw vTaskDelay(1) spins are
// excluded too, structurally rather than by oversight: bb_task REQUIRES
// bb_core (this header includes bb_core.h above), so bb_once routing its
// wait loop through bb_task_delay_ms() would form a component cycle.
// ---------------------------------------------------------------------------

// Pure ms->ticks conversion, parameterized on tick_rate_hz (configTICK_RATE_HZ
// on the espidf shell; any value on host tests) so the tick-granularity
// decision below is host-testable without a FreeRTOS dependency -- mirrors
// bb_task_resolve()'s num_cores parameterization above.
//
// pdMS_TO_TICKS() truncates: at a 100 Hz tick rate (10 ms/tick, the espidf
// default), a caller asking to delay for e.g. 5 ms would silently truncate
// to 0 ticks and not delay at all. This helper guarantees a MINIMUM of 1
// tick for any nonzero `ms`, so "delay some positive amount" never silently
// becomes "don't delay" -- the caller may block slightly longer than
// requested (up to one tick period), never zero. `ms == 0` is a deliberate
// exception: it returns 0 ticks (== vTaskDelay(0), a same-tick yield
// opportunity), matching the caller's explicit "no wait" request rather than
// rounding it up to a real delay. `tick_rate_hz == 0` also returns 0 (no
// valid conversion is possible); the espidf shell never passes 0
// (configTICK_RATE_HZ is a positive build-time constant), this branch exists
// for host-test coverage of the defensive check. The tick_ratehz*ms product
// is computed in 64-bit and clamped to UINT32_MAX before narrowing, so an
// extreme (ms, tick_rate_hz) pair cannot silently wrap to a small tick count.
uint32_t bb_task_delay_ticks_for_ms(uint32_t ms, uint32_t tick_rate_hz);

// Block the calling task for at least `ms` milliseconds. Thin platform
// dispatch over vTaskDelay(bb_task_delay_ticks_for_ms(ms, configTICK_RATE_HZ))
// -- see bb_task_delay_ticks_for_ms() above for the tick-truncation
// guarantee. `ms == 0` yields the current tick slice without blocking
// (vTaskDelay(0) semantics) rather than being rejected as invalid -- this is
// a valid "let other ready tasks run" call, not an error.
//
// Host stub: no real FreeRTOS scheduler exists on host, so this is a no-op
// that returns immediately without sleeping (mirrors bb_task_create()'s host
// stub, which fabricates a handle rather than spinning up a real thread) --
// it proves call sites compile and link, not real timing behaviour.
void bb_task_delay_ms(uint32_t ms);

// Yield the remainder of the current task's time slice to other ready tasks
// of the same priority, without blocking for any minimum duration. Thin
// platform dispatch over taskYIELD(). Kept as a separate primitive from
// bb_task_delay_ms(0) rather than folding into it: FreeRTOS's own
// vTaskDelay() special-cases xTicksToDelay==0 as "force a reschedule" and
// yields via portYIELD_WITHIN_API() rather than blocking, so the two calls
// are not mechanically distinct in general -- on the espidf xtensa port
// portYIELD_WITHIN_API() (esp_crosscore_int_send_yield()) and portYIELD()
// (vPortYield(), what taskYIELD() uses) DO differ, but that's a port-level
// detail, not something this API should lean on. The real reason to keep
// bb_task_yield() separate is call-site readability: a spin-wait that means
// "let other ready tasks run" should read as bb_task_yield(), not as a
// delay call that happens to pass 0.
//
// Host stub: no-op (no real scheduler to yield to), same rationale as
// bb_task_delay_ms() above.
void bb_task_yield(void);

// ---------------------------------------------------------------------------
// Stack high-water-mark accessor -- a LIVE PULL (queries the scheduler
// directly, on demand, every call). Distinct in KIND from the base
// registry's `free_bytes` field above, which is a periodic-scan CACHED
// PUSH (bb_task_base_set_free_bytes() persists whatever the last periodic
// scan happened to observe) -- do not conflate the two, and do not route
// this accessor through the base registry cache.
// ---------------------------------------------------------------------------

// Returns `handle_or_null`'s minimum-ever free stack, in BYTES, since the
// task was created (FreeRTOS's own high-water-mark semantics -- a
// non-increasing value over the task's life). NULL means "the calling
// task" -- matches FreeRTOS's own uxTaskGetStackHighWaterMark(NULL)
// semantics and this repo's existing bb_wdt_task_subscribe_cfg_t.handle ==
// NULL idiom (components/bb_wdt/include/bb_wdt.h). handle_or_null must be NULL
// or a still-live task handle; passing a handle for an already-deleted task
// is undefined behaviour, because FreeRTOS does not validate the handle.
//
// UNIT NOTE: FreeRTOS's uxTaskGetStackHighWaterMark() returns WORDS ("on a
// 32 bit machine a value of 1 means 4 bytes" -- FreeRTOS's own doc), not
// bytes. The espidf shell converts via the shared bb_num_words_to_bytes()
// SSOT (components/bb_num/include/bb_num.h, B1-1256) -- the exact helper
// platform/espidf/bb_task_registry/bb_task_registry_base_scan.c's periodic
// scan already uses for this identical conversion -- rather than
// hand-rolling a second `* sizeof(StackType_t)` multiply.
//
// No Kconfig guard: uxTaskGetStackHighWaterMark() depends only on
// INCLUDE_uxTaskGetStackHighWaterMark, which ESP-IDF's FreeRTOSConfig.h
// (components/freertos/config/include/freertos/FreeRTOSConfig.h) hardcodes
// to 1 unconditionally on every target this repo builds for -- NOT on
// CONFIG_FREERTOS_USE_TRACE_FACILITY. That Kconfig gates a DIFFERENT API,
// uxTaskGetSystemState() (what the base registry's periodic scan above
// uses, and why THAT scan is guarded -- see
// platform/espidf/bb_task_registry/bb_task_registry_base_scan.c's own
// `#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)` guard). Verified
// against the FreeRTOS-Kernel source both ESP-IDF kernel variants ship:
// tasks.c compiles the stack-check helper whenever EITHER
// `configUSE_TRACE_FACILITY == 1` OR `INCLUDE_uxTaskGetStackHighWaterMark ==
// 1` (an OR, not an AND), and the latter is unconditionally 1 in this
// repo's FreeRTOSConfig.h regardless of the trace-facility setting.
//
// Host stub: no real FreeRTOS stack exists on host, so the host build does
// NOT fabricate a plausible-looking number -- it returns a test-injectable
// value only (see bb_task_test_set_stack_hwm_bytes() below, BB_TASK_TESTING
// build), defaulting to 0. Treat a bare host-side reading as "unset, not a
// real measurement" -- never host-test a call site's behavior against this
// value as though it reflected genuine stack headroom.
uint32_t bb_task_stack_hwm_bytes(void *handle_or_null);

#ifdef BB_TASK_TESTING
// Reset the base registry to its initial (empty) state. Test teardown only.
void bb_task_base_test_reset(void);

// Test-only: sets the value the host stub's bb_task_stack_hwm_bytes()
// returns (see its doc above -- host has no real stack to measure).
// Independent of bb_task_base_test_reset() (separate state, not part of the
// base registry) -- defaults to 0 at process start and persists across
// calls until reassigned.
void bb_task_test_set_stack_hwm_bytes(uint32_t bytes);

// Test-only: returns the `handle_or_null` argument passed to the most
// recent bb_task_stack_hwm_bytes() call on the host stub, so a test can
// assert NULL was forwarded through unchanged (the "calling task" case)
// rather than substituted for something else.
void *bb_task_test_last_stack_hwm_handle(void);

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
