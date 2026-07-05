// bb_task_registry — thin consumer of the generic bb_registry primitive.
// Compiled on both host (tests) and ESP-IDF as part of the bb_task_registry
// component.

#include "bb_task_registry.h"
#include "bb_registry.h"
#include "bb_log.h"
#include "bb_clock.h"
#include "bb_wdt.h"
#include "bb_task.h"
#include "bb_str.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

/* Kconfig bridge: honour CONFIG_BB_TASK_REGISTRY_MAX from build flags; default 24. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_TASK_REGISTRY_WDT_SUPPORT
#define BB_TASK_REGISTRY_WDT_SUPPORT CONFIG_BB_TASK_REGISTRY_WDT_SUPPORT
#endif
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#ifdef CONFIG_BB_TASK_REGISTRY_MAX
#define BB_TASK_REGISTRY_MAX CONFIG_BB_TASK_REGISTRY_MAX
#endif
#ifndef BB_TASK_REGISTRY_MAX
#define BB_TASK_REGISTRY_MAX 24
#endif
#ifndef BB_TASK_REGISTRY_WDT_SUPPORT
#define BB_TASK_REGISTRY_WDT_SUPPORT 1
#endif

static const char *TAG = "bb_task_registry";

// Payload pool — bb_registry stores name->void*; this pool backs the void*
// values with the extra fields (stack budget, wdt state, handle) this
// component needs. One slot per registry capacity slot. `index` in a
// bb_task_registry_token_t is the index into this pool (stable for the
// lifetime of a registration; generation invalidates a token once the slot
// is deregistered and possibly reused).
typedef struct {
    void          *handle;
    uint32_t       stack_budget_bytes;
    bool           wdt_subscribed;
    bool           in_use;
    _Atomic uint16_t generation;
    // Relative timestamp (ms) of the last feed, NOT an absolute/exposed
    // timestamp — a 32-bit atomic IS lock-free on Xtensa (unlike 64-bit),
    // so this stays uint32_t. Consumers compute age via the wrap-safe
    // delta (uint32_t)(now_ms - last_feed_ms); watchdog-timeout-scale ages
    // are unaffected by the ~49.7-day wrap, so the ms64 absolute-timestamp
    // rule (bb_clock.h) does not apply to this field.
    _Atomic uint32_t last_feed_ms;

    // Software watchdog (B1-458 PR-B). sw_wdt_timeout_ms is set once, at
    // register() time, under s_task_reg_lock; miss_count/last_miss_ms/
    // miss_active are written ONLY by bb_task_registry_sw_wdt_check()'s
    // writeback phase (under s_task_reg_lock) and read under the same lock
    // (snapshot phase / lookup) — single writer (the monitor), all access
    // under lock, so these are plain fields (no atomics needed).
    uint32_t sw_wdt_timeout_ms;
    uint32_t miss_count;
    uint32_t last_miss_ms;
    bool     miss_active;
} bb_task_entry_t;

static bb_task_entry_t s_pool[BB_TASK_REGISTRY_MAX];

// Overflow observability (B1-471). s_dropped counts every register()/
// test_seed() call rejected because the pool was full. s_hwm_warned fires
// the warning once, mirroring bb_registry_register's own fire-once HWM idiom
// (components/bb_registry) and bb_dispatch_api_add's CAP-margin idiom
// (components/bb_http/src/bb_dispatch_api.c).
static uint32_t s_dropped;
static bool     s_hwm_warned;

// Headroom (slots remaining) at which the one-shot HWM warning fires. Kept
// small and margin-based (rather than a fixed absolute count) because
// BB_TASK_REGISTRY_MAX ranges 1..64 (Kconfig) — an absolute threshold like
// CAP-8 could exceed capacity on a small pool.
#define BB_TASK_REGISTRY_HWM_MARGIN 2

// Zero-token safety: BB_TASK_REGISTRY_TOKEN_INVALID / a zero-initialized
// bb_task_registry_token_t carries {index=0, generation=0}. Every slot's
// generation is initialized to 1 (never 0) so a live slot 0 can never be
// aliased by an all-zero token — real generations issued by register() are
// always >= 1.
static void pool_init_generations(void)
{
    for (int i = 0; i < BB_TASK_REGISTRY_MAX; i++) {
        atomic_store_explicit(&s_pool[i].generation, 1, memory_order_relaxed);
    }
}

// Runs once at static-init time (before app_main/tests), via a constructor
// so the invariant holds even for the very first register() call.
__attribute__((constructor))
static void pool_init_generations_ctor(void)
{
    pool_init_generations();
}

BB_REGISTRY_DEFINE_TAGGED(s_task_registry, BB_TASK_REGISTRY_MAX, "tasks");

// Wrapper mutex — serialises the public ops as a single atomic unit over the
// top of the primitive's own internal lock, and also protects the payload
// pool (s_pool) which the primitive knows nothing about. Mirrors
// bb_ring_registry's s_ring_reg_lock pattern (see that file for the full
// TOCTOU/UAF rationale) — always taken OUTSIDE any bb_registry_* call, so
// there is no lock-order inversion.
static pthread_mutex_t s_task_reg_lock = PTHREAD_MUTEX_INITIALIZER;

// Single source of truth for "slots occupied": scan s_pool.in_use. Always
// called under s_task_reg_lock. This replaces a formerly-independent
// s_pool_count shadow counter (B1-471 follow-up) — both the public
// bb_task_registry_count() and the HWM threshold check below read this same
// function, so there is exactly one place that knows the occupancy count.
// BB_TASK_REGISTRY_MAX is at most 64 (Kconfig-bounded), so an O(n) scan
// under the lock is cheap relative to the mutex itself.
static uint16_t count_locked(void)
{
    uint16_t count = 0;
    for (int i = 0; i < BB_TASK_REGISTRY_MAX; i++) {
        if (s_pool[i].in_use) {
            count++;
        }
    }
    return count;
}

static int pool_alloc_locked(void)
{
    for (int i = 0; i < BB_TASK_REGISTRY_MAX; i++) {
        if (!s_pool[i].in_use) {
            s_pool[i].in_use = true;

            // High-watermark warn: fire once when the pool crosses within
            // BB_TASK_REGISTRY_HWM_MARGIN slots of capacity. This fires on
            // the raw slot allocation (before the caller's bb_registry_
            // register() dup-name check, if any, resolves) — a duplicate-
            // name rollback right at the margin is a rare, purely cosmetic
            // early fire and is preferable to deferring the check until
            // after bb_registry_register() resolves.
            uint16_t occupied = count_locked();
            uint16_t threshold = (BB_TASK_REGISTRY_MAX > BB_TASK_REGISTRY_HWM_MARGIN)
                                  ? (uint16_t)(BB_TASK_REGISTRY_MAX - BB_TASK_REGISTRY_HWM_MARGIN)
                                  : (uint16_t)BB_TASK_REGISTRY_MAX;
            if (!s_hwm_warned && occupied >= threshold) {
                s_hwm_warned = true;
                bb_log_w(TAG, "high-watermark: %"PRIu16"/%d tasks registered",
                         occupied, BB_TASK_REGISTRY_MAX);
            }
            return i;
        }
    }
    s_dropped++;
    return -1;
}

// All call sites pass an entry obtained from pool_alloc_locked (register(),
// test_seed()) or a non-NULL scan.found_entry (deregister()) — entry is
// never NULL here, so this is an unconditional clear rather than a guarded
// one (avoids an untestable dead branch).
static void pool_free_locked(bb_task_entry_t *entry)
{
    entry->in_use = false;
}

// Best-effort attempt to subscribe `handle` to the hardware Task WDT.
// Returns true on success. Never fails registration on error — the caller
// logs and leaves wdt_subscribed=false.
//
// ESP-IDF has no handle-taking bb_wdt API (only the SELF-subscribe
// esp_task_wdt_add(NULL) wrapped by bb_wdt_task_subscribe/unsubscribe), so
// the parent-context handle-taking calls are made directly here via
// esp_task_wdt_add/delete, gated by BB_TASK_REGISTRY_WDT_SUPPORT.
//
// On host there is no real handle or Task WDT to subscribe; the existing
// bb_wdt self-subscribe host stub is reused as the observable stand-in so
// host tests can assert subscribe/unsubscribe/feed counts via
// bb_wdt_test_subscribe_count()/bb_wdt_test_unsubscribe_count().
#if defined(ESP_PLATFORM) && BB_TASK_REGISTRY_WDT_SUPPORT
static bool hw_wdt_subscribe(void *handle)
{
    esp_err_t err = esp_task_wdt_add((TaskHandle_t)handle);
    if (err != ESP_OK) {
        bb_log_w(TAG, "esp_task_wdt_add failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void hw_wdt_unsubscribe(void *handle)
{
    esp_err_t err = esp_task_wdt_delete((TaskHandle_t)handle);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        bb_log_w(TAG, "esp_task_wdt_delete failed: %s", esp_err_to_name(err));
    }
}
#elif defined(ESP_PLATFORM)
static bool hw_wdt_subscribe(void *handle)
{
    (void)handle;
    return false;
}

static void hw_wdt_unsubscribe(void *handle)
{
    (void)handle;
}
#else
static bool hw_wdt_subscribe(void *handle)
{
    (void)handle;
    return bb_wdt_task_subscribe() == BB_OK;
}

static void hw_wdt_unsubscribe(void *handle)
{
    (void)handle;
    bb_wdt_task_unsubscribe();
}
#endif

bb_err_t bb_task_registry_register(const char *name, uint32_t stack_budget_bytes, void *handle,
                                    const bb_task_registry_opts_t *opts,
                                    bb_task_registry_token_t *out_token)
{
    if (!name) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&s_task_reg_lock);

    int idx = pool_alloc_locked();
    if (idx < 0) {
        pthread_mutex_unlock(&s_task_reg_lock);
        bb_log_w(TAG, "register('%s') failed: pool exhausted", name);
        return BB_ERR_NO_SPACE;
    }

    bb_task_entry_t *entry = &s_pool[idx];
    entry->handle              = handle;
    entry->stack_budget_bytes  = stack_budget_bytes;
    entry->wdt_subscribed      = false;
    entry->sw_wdt_timeout_ms   = opts ? opts->sw_wdt_timeout_ms : 0;
    entry->miss_count          = 0;
    entry->last_miss_ms        = 0;
    entry->miss_active         = false;
    atomic_store_explicit(&entry->last_feed_ms, 0, memory_order_relaxed);

    // Order: pool slot confirmed BEFORE the hw_wdt_add call.
    if (opts && opts->hw_wdt_subscribe) {
        if (handle) {
            entry->wdt_subscribed = hw_wdt_subscribe(handle);
        } else {
            bb_log_w(TAG, "hw_wdt_subscribe requested for '%s' with NULL handle — skipped", name);
        }
    }

    bb_err_t err = bb_registry_register(&s_task_registry, name, entry);
    if (err != BB_OK) {
        if (entry->wdt_subscribed) {
            hw_wdt_unsubscribe(handle);
            entry->wdt_subscribed = false;
        }
        pool_free_locked(entry);
    }

    // seqlock/generation protocol: the entry payload (handle,
    // wdt_subscribed, stack_budget_bytes) is fully written above BEFORE
    // this release-store publishes the generation. bb_task_registry_feed()
    // acquire-loads generation before reading that payload, so a feed()
    // observing this generation value is guaranteed to see the payload
    // written here (release/acquire pairing), not a torn/partial write.
    uint16_t generation = atomic_load_explicit(&entry->generation, memory_order_relaxed);
    atomic_store_explicit(&entry->generation, generation, memory_order_release);

    pthread_mutex_unlock(&s_task_reg_lock);

    if (err != BB_OK) {
        bb_log_w(TAG, "register('%s') failed: %d", name, (int)err);
    } else {
        if (out_token) {
            out_token->index      = (uint16_t)idx;
            out_token->generation = generation;
        }
        // Base-registry join (task-registry unification PR3): ensure a base
        // entry exists for `handle`, linking the overlay's authoritative
        // budget/wdt state to it -- create-if-absent, update-if-present
        // (bb_task_base_upsert never double-inserts). This is separate from
        // (and always wins over) any best-effort placeholder the periodic
        // base scan may have already inserted for this handle. Only when
        // handle is non-NULL -- bb_task_base_upsert requires a real handle.
        if (handle) {
            bb_task_base_upsert(handle, name, stack_budget_bytes, entry->wdt_subscribed);
        }
    }
    return err;
}

// find_by_handle scan state — used by bb_task_registry_deregister below.
typedef struct {
    void       *target;
    const char *found_name;
    bb_task_entry_t *found_entry;
} find_by_handle_t;

static void find_by_handle_cb(const char *name, void *value, void *ctx)
{
    find_by_handle_t *scan = (find_by_handle_t *)ctx;
    bb_task_entry_t *entry = (bb_task_entry_t *)value;
    if (!scan->found_name && entry->handle == scan->target) {
        scan->found_name  = name;
        scan->found_entry = entry;
    }
}

bb_err_t bb_task_registry_deregister(void *handle)
{
    if (!handle) {
        return BB_ERR_INVALID_ARG;
    }

    // Whole find+deregister sequence runs under s_task_reg_lock so it is
    // atomic with respect to concurrent register()/deregister()/foreach()
    // calls (same TOCTOU-closing rationale as bb_ring_registry).
    pthread_mutex_lock(&s_task_reg_lock);

    find_by_handle_t scan = { .target = handle, .found_name = NULL, .found_entry = NULL };
    bb_registry_foreach(&s_task_registry, find_by_handle_cb, &scan);

    bb_err_t err;
    if (!scan.found_name) {
        err = BB_ERR_NOT_FOUND;
    } else {
        // scan.found_name was resolved from this same table under this same
        // lock, and s_task_registry is never frozen (bb_registry_freeze is
        // not called on it), so bb_registry_deregister cannot fail here —
        // free the pool slot unconditionally rather than gate on an
        // unreachable (and therefore untestable) error branch.
        err = bb_registry_deregister(&s_task_registry, scan.found_name);
        if (scan.found_entry->wdt_subscribed) {
            hw_wdt_unsubscribe(handle);
            scan.found_entry->wdt_subscribed = false;
        }
        // Bump generation BEFORE freeing the slot so any outstanding token
        // referencing this slot (held by a task that has not yet noticed
        // the deregister) is invalidated even if the slot is immediately
        // reused by a subsequent register(). memory_order_release pairs
        // with the memory_order_acquire load in bb_task_registry_feed() —
        // see the seqlock/generation protocol comment in register().
        atomic_fetch_add_explicit(&scan.found_entry->generation, 1, memory_order_release);
        pool_free_locked(scan.found_entry);
    }

    pthread_mutex_unlock(&s_task_reg_lock);

    if (err == BB_OK) {
        // Base-registry unjoin (task-registry unification PR3): proactively
        // remove the base entry rather than waiting for the periodic base
        // scan's grace-window sweep to notice the handle is gone -- closes
        // the window where a fast-recycled TaskHandle_t could otherwise
        // alias a stale base entry before the next scan runs. Best-effort:
        // bb_task_base_remove's own BB_ERR_NOT_FOUND (handle was never
        // base-registered, e.g. register() was called with a NULL handle)
        // is not an error here.
        bb_task_base_remove(handle);
    }
    return err;
}

// Feed the hardware Task WDT for `token`. LOCK-FREE — does NOT take
// s_task_reg_lock. SELF-FEED ONLY on ESP-IDF — see header for the full
// contract and rationale.
void bb_task_registry_feed(bb_task_registry_token_t token)
{
    if (token.index >= BB_TASK_REGISTRY_MAX) {
        return;
    }
    bb_task_entry_t *entry = &s_pool[token.index];
    // seqlock/generation protocol: acquire-load generation BEFORE reading
    // any non-atomic payload field (wdt_subscribed, handle) below. This
    // pairs with the release-store in register()/deregister() so that once
    // this load observes a given generation value, the payload writes that
    // preceded that generation's release-store are guaranteed visible here.
    uint16_t generation = atomic_load_explicit(&entry->generation, memory_order_acquire);
    if (generation != token.generation) {
        return;  // stale/reused slot — no-op
    }
    atomic_store_explicit(&entry->last_feed_ms, bb_clock_now_ms(), memory_order_relaxed);
    if (entry->wdt_subscribed) {
#ifdef ESP_PLATFORM
        if ((TaskHandle_t)entry->handle == xTaskGetCurrentTaskHandle()) {
            bb_wdt_task_feed();
        } else {
            // No task name is available here (lock-free path; the name
            // lives in s_task_registry keyed by name, not in this entry) —
            // log the handle for correlation instead.
            bb_log_w(TAG, "feed called cross-task for handle %p — hw wdt not fed", entry->handle);
        }
#else
        bb_wdt_task_feed();
#endif
    }
}

// --- Software watchdog monitor (B1-458 PR-B) -------------------------------

static bb_task_registry_sw_wdt_handler_t s_sw_wdt_handler     = NULL;
static void                              *s_sw_wdt_handler_ctx = NULL;

void bb_task_registry_set_sw_wdt_handler(bb_task_registry_sw_wdt_handler_t fn, void *ctx)
{
    s_sw_wdt_handler     = fn;
    s_sw_wdt_handler_ctx = ctx;
}

// Snapshot row copied out under s_task_reg_lock in phase 1 of
// bb_task_registry_sw_wdt_check(), consumed lock-free in phase 2, and used to
// guard the phase-3 writeback against a slot deregistered/reused in between.
typedef struct {
    uint16_t index;
    uint16_t generation;
    char     name[BB_TASK_REGISTRY_NAME_MAX];
    void    *handle;
    uint32_t sw_wdt_timeout_ms;
    uint32_t last_feed_ms;
    uint32_t miss_count;
    uint32_t last_miss_ms;
    bool     miss_active;
    bool     dirty;  // phase 2 staged a miss_count/last_miss_ms/miss_active change
} sw_wdt_snapshot_row_t;

typedef struct {
    sw_wdt_snapshot_row_t *rows;
    int                     n;
    int                     max;
} sw_wdt_snapshot_ctx_t;

// Runs under s_task_reg_lock (via bb_registry_foreach) — copy only, no I/O,
// no allocation, no handler invocation (mirrors bb_ring_diag's snapshot
// callback contract).
static void sw_wdt_snapshot_cb(const char *name, void *value, void *ctx)
{
    sw_wdt_snapshot_ctx_t *sc = (sw_wdt_snapshot_ctx_t *)ctx;
    if (sc->n >= sc->max) {
        // Defensive only, mirrors bb_ring_diag's identical guard: sc->max is
        // BB_TASK_REGISTRY_MAX, the SAME constant that bounds the pool this
        // foreach iterates, so sc->n cannot reach sc->max mid-iteration —
        // not reachable/testable given the current 1:1 sizing.
        return;
    }
    bb_task_entry_t *entry = (bb_task_entry_t *)value;
    if (entry->sw_wdt_timeout_ms == 0) {
        return;  // software watchdog off for this task
    }
    sw_wdt_snapshot_row_t *row = &sc->rows[sc->n];
    row->index             = (uint16_t)(entry - s_pool);
    row->generation         = atomic_load_explicit(&entry->generation, memory_order_relaxed);
    // bb_registry never invokes foreach callbacks with a NULL name (only
    // successfully-registered, non-NULL-keyed entries are iterated) — no
    // defensive NULL check, matching foreach_trampoline's contract above.
    bb_strlcpy(row->name, name, sizeof(row->name));
    row->handle             = entry->handle;
    row->sw_wdt_timeout_ms  = entry->sw_wdt_timeout_ms;
    row->last_feed_ms       = atomic_load_explicit(&entry->last_feed_ms, memory_order_relaxed);
    row->miss_count         = entry->miss_count;
    row->last_miss_ms       = entry->last_miss_ms;
    row->miss_active        = entry->miss_active;
    row->dirty              = false;
    sc->n++;
}

// Snapshot buffer for bb_task_registry_sw_wdt_check(). File-static rather
// than a stack local: BB_TASK_REGISTRY_MAX is an independent Kconfig (up to
// 64) and a stack array of that size would make the monitor task's stack
// budget (BB_TASK_REGISTRY_SW_WDT_STACK) couple to it, risking overflow at
// high MAX. bb_task_registry_sw_wdt_check() has a SINGLE caller at runtime
// (the sw-wdt monitor task, a singleton) and host tests invoke it
// synchronously on one thread — it is NOT reentrant, so a shared static
// buffer is safe. This moves ~BB_TASK_REGISTRY_MAX*sizeof(row) into BSS
// (bounded, predictable) instead of the caller's stack, with no per-tick
// heap use.
static sw_wdt_snapshot_row_t s_sw_wdt_snapshot[BB_TASK_REGISTRY_MAX];

void bb_task_registry_sw_wdt_check(uint32_t now_ms)
{
    // Phase 1 — snapshot under lock, static buffer (see s_sw_wdt_snapshot
    // above), no I/O.
    sw_wdt_snapshot_ctx_t sc = { .rows = s_sw_wdt_snapshot, .n = 0, .max = BB_TASK_REGISTRY_MAX };

    pthread_mutex_lock(&s_task_reg_lock);
    bb_registry_foreach(&s_task_registry, sw_wdt_snapshot_cb, &sc);
    pthread_mutex_unlock(&s_task_reg_lock);

    // Phase 2 — evaluate lock-free. The handler callback runs here, OUTSIDE
    // the lock, per the header contract.
    for (int i = 0; i < sc.n; i++) {
        sw_wdt_snapshot_row_t *row = &s_sw_wdt_snapshot[i];
        bool overdue = (uint32_t)(now_ms - row->last_feed_ms) > row->sw_wdt_timeout_ms;
        if (overdue && !row->miss_active) {
            uint32_t overrun_ms = (uint32_t)(now_ms - row->last_feed_ms) - row->sw_wdt_timeout_ms;
            bb_log_w(TAG, "task '%s' missed software watchdog by %"PRIu32" ms", row->name, overrun_ms);
            if (s_sw_wdt_handler) {
                s_sw_wdt_handler(row->name, row->handle, overrun_ms, s_sw_wdt_handler_ctx);
            }
            row->miss_count += 1;
            row->last_miss_ms = now_ms;
            row->miss_active  = true;
            row->dirty        = true;
        } else if (!overdue && row->miss_active) {
            row->miss_active = false;
            row->dirty       = true;
        }
    }

    // Phase 3 — writeback under lock, generation-guarded so a deregister
    // that happened between phases 1 and 3 is a safe no-op rather than a
    // stale/corrupting write into a reused slot.
    pthread_mutex_lock(&s_task_reg_lock);
    for (int i = 0; i < sc.n; i++) {
        sw_wdt_snapshot_row_t *row = &s_sw_wdt_snapshot[i];
        if (!row->dirty) {
            continue;
        }
        bb_task_entry_t *entry = &s_pool[row->index];
        uint16_t cur_generation = atomic_load_explicit(&entry->generation, memory_order_relaxed);
        if (cur_generation != row->generation) {
            continue;  // slot deregistered/reused since the snapshot — skip
        }
        entry->miss_count   = row->miss_count;
        entry->last_miss_ms = row->last_miss_ms;
        entry->miss_active  = row->miss_active;
    }
    pthread_mutex_unlock(&s_task_reg_lock);
}

bool bb_task_registry_lookup_sw_wdt(const char *name, uint32_t now_ms,
                                     uint32_t *out_timeout_ms,
                                     uint32_t *out_last_feed_age_ms,
                                     uint32_t *out_last_miss_age_ms,
                                     uint32_t *out_miss_count)
{
    if (!name) {
        return false;
    }

    pthread_mutex_lock(&s_task_reg_lock);
    void *value = bb_registry_lookup(&s_task_registry, name);
    if (!value) {
        pthread_mutex_unlock(&s_task_reg_lock);
        return false;
    }
    const bb_task_entry_t *entry = (const bb_task_entry_t *)value;
    if (entry->sw_wdt_timeout_ms == 0) {
        pthread_mutex_unlock(&s_task_reg_lock);
        return false;
    }
    if (out_timeout_ms) {
        *out_timeout_ms = entry->sw_wdt_timeout_ms;
    }
    if (out_last_feed_age_ms) {
        uint32_t last_feed_ms = atomic_load_explicit(&entry->last_feed_ms, memory_order_relaxed);
        *out_last_feed_age_ms = (uint32_t)(now_ms - last_feed_ms);
    }
    if (out_last_miss_age_ms) {
        *out_last_miss_age_ms = entry->miss_count > 0 ? (uint32_t)(now_ms - entry->last_miss_ms) : 0;
    }
    if (out_miss_count) {
        *out_miss_count = entry->miss_count;
    }
    pthread_mutex_unlock(&s_task_reg_lock);
    return true;
}

uint16_t bb_task_registry_count(void)
{
    pthread_mutex_lock(&s_task_reg_lock);
    uint16_t count = count_locked();
    pthread_mutex_unlock(&s_task_reg_lock);
    return count;
}

uint16_t bb_task_registry_capacity(void)
{
    return BB_TASK_REGISTRY_MAX;
}

uint32_t bb_task_registry_dropped(void)
{
    pthread_mutex_lock(&s_task_reg_lock);
    uint32_t dropped = s_dropped;
    pthread_mutex_unlock(&s_task_reg_lock);
    return dropped;
}

bool bb_task_registry_lookup_budget(const char *name, uint32_t *out_budget, bool *out_wdt)
{
    if (!name) {
        return false;
    }

    pthread_mutex_lock(&s_task_reg_lock);
    void *value = bb_registry_lookup(&s_task_registry, name);
    if (!value) {
        pthread_mutex_unlock(&s_task_reg_lock);
        return false;
    }
    bb_task_entry_t *entry = (bb_task_entry_t *)value;
    if (out_budget) {
        *out_budget = entry->stack_budget_bytes;
    }
    if (out_wdt) {
        *out_wdt = entry->wdt_subscribed;
    }
    pthread_mutex_unlock(&s_task_reg_lock);
    return true;
}

// Trampoline: bridges bb_registry_foreach's (name, void*, ctx) callback shape
// to bb_task_registry_foreach's (name, budget, wdt, ctx) shape.
typedef struct {
    bb_task_registry_cb_t cb;
    void                  *ctx;
} foreach_bridge_t;

static void foreach_trampoline(const char *name, void *value, void *ctx)
{
    foreach_bridge_t *bridge = (foreach_bridge_t *)ctx;
    bb_task_entry_t *entry = (bb_task_entry_t *)value;
    bridge->cb(name, entry->stack_budget_bytes, entry->wdt_subscribed, bridge->ctx);
}

void bb_task_registry_foreach(bb_task_registry_cb_t cb, void *ctx)
{
    if (!cb) {
        return;
    }
    // s_task_reg_lock is held across the ENTIRE call, including every
    // invocation of the caller's cb — see bb_ring_registry.h's foreach
    // contract for the full rationale.
    pthread_mutex_lock(&s_task_reg_lock);
    foreach_bridge_t bridge = { .cb = cb, .ctx = ctx };
    bb_registry_foreach(&s_task_registry, foreach_trampoline, &bridge);
    pthread_mutex_unlock(&s_task_reg_lock);
}

#ifdef BB_TASK_REGISTRY_TESTING
void bb_task_registry_test_reset(void)
{
    pthread_mutex_lock(&s_task_reg_lock);
    bb_registry_reset(&s_task_registry);
    memset(s_pool, 0, sizeof(s_pool));
    s_dropped     = 0;
    s_hwm_warned  = false;
    s_sw_wdt_handler     = NULL;
    s_sw_wdt_handler_ctx = NULL;
    // memset zeroes generation too — restore the "never 0" invariant (see
    // pool_init_generations) so a zero-initialized token can never alias a
    // live slot 0 across test cases.
    pool_init_generations();
    pthread_mutex_unlock(&s_task_reg_lock);
}

bb_err_t bb_task_registry_test_seed(const char *name, uint32_t stack_budget_bytes, bool wdt_subscribed,
                                     bb_task_registry_token_t *out_token)
{
    if (!name) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&s_task_reg_lock);
    int idx = pool_alloc_locked();
    if (idx < 0) {
        pthread_mutex_unlock(&s_task_reg_lock);
        return BB_ERR_NO_SPACE;
    }
    bb_task_entry_t *entry = &s_pool[idx];
    entry->handle             = NULL;
    entry->stack_budget_bytes = stack_budget_bytes;
    entry->wdt_subscribed     = wdt_subscribed;
    entry->sw_wdt_timeout_ms  = 0;
    entry->miss_count         = 0;
    entry->last_miss_ms       = 0;
    entry->miss_active        = false;
    atomic_store_explicit(&entry->last_feed_ms, 0, memory_order_relaxed);

    bb_err_t err = bb_registry_register(&s_task_registry, name, entry);
    if (err != BB_OK) {
        pool_free_locked(entry);
    }

    // Same publish-point release-store as register() — see the
    // seqlock/generation protocol comment there.
    uint16_t generation = atomic_load_explicit(&entry->generation, memory_order_relaxed);
    atomic_store_explicit(&entry->generation, generation, memory_order_release);

    pthread_mutex_unlock(&s_task_reg_lock);

    if (err == BB_OK && out_token) {
        out_token->index      = (uint16_t)idx;
        out_token->generation = generation;
    }
    return err;
}

void bb_task_registry_test_set_last_feed_ms(bb_task_registry_token_t token, uint32_t ms)
{
    if (token.index >= BB_TASK_REGISTRY_MAX) {
        return;
    }
    atomic_store_explicit(&s_pool[token.index].last_feed_ms, ms, memory_order_relaxed);
}
#endif
