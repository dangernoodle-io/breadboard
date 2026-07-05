// bb_task_registry — pure base-scan evaluator (task-registry unification
// PR3). Portable: no FreeRTOS types. Compiled on both host (tests) and
// ESP-IDF as part of the bb_task_registry component. Coverage-GATED SSOT --
// every branch here must be host-tested (see test/test_host/
// test_bb_task_registry_base_scan.c).
//
// Single caller at runtime (the periodic base-scan job, a singleton
// espidf-side timer callback); host tests invoke it synchronously on one
// thread -- NOT reentrant, mirrors bb_task_registry_sw_wdt_check()'s own
// single-caller rationale (see bb_task_registry.c).

#include "bb_task_registry_base_scan.h"
#include "bb_task.h"

#include <string.h>

// Local alias for readability only -- BB_TASK_BASE_MAX_CAP (bb_task.h) is
// the ONE real symbol both this file's tables (low-stack debounce table,
// base_reconcile_ctx_t's fixed-size buffers below) and bb_task's own pool
// are sized from, so they cannot silently diverge (the low-stack debounce
// table can never need more slots than the base registry itself can hold --
// bb_task_base_upsert()/bb_task_base_touch_or_insert() return
// BB_ERR_NO_SPACE beyond it). No separate CONFIG_BB_TASK_BASE_MAX
// re-derivation here on purpose -- that would reintroduce exactly the
// two-macro divergence risk this alias exists to close.
#define BB_TASK_REGISTRY_BASE_MAX BB_TASK_BASE_MAX_CAP

// Compile-time guard: fails the build (rather than silently overflowing
// base_reconcile_cb's fixed-size buffers) if this alias is ever changed to
// diverge from bb_task's own exported capacity.
_Static_assert(BB_TASK_REGISTRY_BASE_MAX == BB_TASK_BASE_MAX_CAP,
               "bb_task_registry's base-scan capacity must match bb_task's base registry capacity");

// ---------------------------------------------------------------------------
// Low-stack handler registration
// ---------------------------------------------------------------------------

static bb_task_registry_low_stack_handler_t s_low_handler        = NULL;
static uint32_t                             s_low_threshold_bytes = 0;
static void                                 *s_low_handler_ctx    = NULL;

void bb_task_registry_set_low_stack_handler(bb_task_registry_low_stack_handler_t fn,
                                             uint32_t threshold_bytes, void *ctx)
{
    s_low_handler         = fn;
    s_low_threshold_bytes = threshold_bytes;
    s_low_handler_ctx     = ctx;
}

// ---------------------------------------------------------------------------
// Low-stack debounce table -- handle-keyed mark-and-sweep, mirrors the
// grace-window rationale of bb_task_base_sweep_apply / the retired
// bb_health_stack_table_sweep. Single-caller (see file header) so no lock is
// needed.
// ---------------------------------------------------------------------------

#define BB_TASK_REGISTRY_LOW_STACK_SWEEP_GRACE 1

typedef struct {
    void     *handle;
    bool      in_use;
    bool      low;
    uint32_t  seen_tick;
} low_stack_entry_t;

static low_stack_entry_t s_low_states[BB_TASK_REGISTRY_BASE_MAX];

// Find-or-insert `handle` in s_low_states, tag it with scan_tick. Returns
// NULL if handle is new and the table is full of distinct live handles.
static low_stack_entry_t *low_state_mark(void *handle, uint32_t scan_tick)
{
    for (int i = 0; i < BB_TASK_REGISTRY_BASE_MAX; i++) {
        if (s_low_states[i].in_use && s_low_states[i].handle == handle) {
            s_low_states[i].seen_tick = scan_tick;
            return &s_low_states[i];
        }
    }
    for (int i = 0; i < BB_TASK_REGISTRY_BASE_MAX; i++) {
        if (!s_low_states[i].in_use) {
            s_low_states[i].handle    = handle;
            s_low_states[i].in_use    = true;
            s_low_states[i].low       = false;
            s_low_states[i].seen_tick = scan_tick;
            return &s_low_states[i];
        }
    }
    return NULL;
}

// Free entries missed for more than BB_TASK_REGISTRY_LOW_STACK_SWEEP_GRACE
// consecutive scans (their task has exited).
static void low_state_sweep(uint32_t scan_tick)
{
    for (int i = 0; i < BB_TASK_REGISTRY_BASE_MAX; i++) {
        if (s_low_states[i].in_use && s_low_states[i].seen_tick != scan_tick &&
            (scan_tick - s_low_states[i].seen_tick) > BB_TASK_REGISTRY_LOW_STACK_SWEEP_GRACE) {
            memset(&s_low_states[i], 0, sizeof(s_low_states[i]));
        }
    }
}

// ---------------------------------------------------------------------------
// Base-registry reconcile (bb_task_base_sweep_apply writeback)
// ---------------------------------------------------------------------------

typedef struct {
    bb_task_base_entry_t entries[BB_TASK_REGISTRY_BASE_MAX];
    void                 *handles[BB_TASK_REGISTRY_BASE_MAX];
    int                   n;
} base_reconcile_ctx_t;

// Runs under bb_task_base_foreach's lock -- copy only, no I/O, no
// bb_task_base_* re-entry (mirrors bb_task_base_foreach's own contract).
// BB_TASK_REGISTRY_BASE_MAX is the SAME constant that bounds bb_task's own
// base registry pool this foreach iterates (bb_task_base_upsert itself
// returns BB_ERR_NO_SPACE beyond it), so rc->n can never exceed
// BB_TASK_REGISTRY_BASE_MAX-1 entering this callback -- write unconditionally
// rather than gate on an unreachable (and therefore untestable) capacity
// check (mirrors bb_task_base_upsert/_remove's identical "register
// unconditionally" rationale in components/bb_task/src/bb_task_common.c).
static void base_reconcile_cb(void *handle, const bb_task_base_entry_t *entry, void *ctx)
{
    base_reconcile_ctx_t *rc = (base_reconcile_ctx_t *)ctx;
    rc->handles[rc->n] = handle;
    rc->entries[rc->n] = *entry;
    rc->n++;
}

// ---------------------------------------------------------------------------
// bb_task_registry_base_scan_apply
// ---------------------------------------------------------------------------

int bb_task_registry_base_scan_apply(const bb_task_registry_base_row_t *rows, int n,
                                      uint32_t now_tick)
{
    // NOTE: unlike bb_task_base_sweep_apply's own null/n<=0 guard (which
    // legitimately no-ops the whole call), a scan with zero live rows must
    // still run the base-registry sweep/reconcile below -- rows/n only
    // describe what is alive THIS scan; a genuinely empty scan is exactly
    // the case that must reclaim every previously-tracked handle once their
    // grace window expires. Only the per-row mark/low-stack loop is
    // skipped.
    if (rows && n > 0) {
        for (int i = 0; i < n; i++) {
            void *handle = rows[i].handle;
            if (!handle) {
                continue;
            }

            // 1. base-registry mark: a SINGLE atomic touch-or-insert --
            // touch if already tracked, else insert a best-effort
            // placeholder (budget unknown, wdt_arm=false). Never clobbers a
            // handle already tracked via bb_task_create()/
            // bb_task_registry_register()'s overlay-join: unlike a
            // separately locked touch() + conditional upsert() pair, this
            // cannot lose a race to a concurrent creator inserting the REAL
            // entry in the gap (see bb_task_base_touch_or_insert, bb_task.h).
            bb_task_base_touch_or_insert(handle, rows[i].name, now_tick);

            // 2. low-stack transition, debounced by handle.
            if (s_low_handler) {
                bool is_low = rows[i].free_bytes < s_low_threshold_bytes;
                low_stack_entry_t *st = low_state_mark(handle, now_tick);
                if (st) {
                    if (is_low && !st->low) {
                        s_low_handler(rows[i].name, handle, rows[i].free_bytes, s_low_handler_ctx);
                    }
                    st->low = is_low;
                }
            }
        }
    }

    if (s_low_handler) {
        low_state_sweep(now_tick);
    }

    // 3. base-registry sweep/reconcile: snapshot, evaluate off the live
    // registry, then remove every handle the evaluator reclaimed.
    base_reconcile_ctx_t rc = { .n = 0 };
    bb_task_base_foreach(base_reconcile_cb, &rc);

    int freed = bb_task_base_sweep_apply(rc.entries, rc.n, now_tick);
    if (freed > 0) {
        for (int i = 0; i < rc.n; i++) {
            if (!rc.entries[i].in_use) {
                bb_task_base_remove(rc.handles[i]);
            }
        }
    }
    return freed;
}

#ifdef BB_TASK_REGISTRY_TESTING
void bb_task_registry_base_scan_test_reset(void)
{
    s_low_handler         = NULL;
    s_low_threshold_bytes = 0;
    s_low_handler_ctx     = NULL;
    memset(s_low_states, 0, sizeof(s_low_states));
}
#endif
