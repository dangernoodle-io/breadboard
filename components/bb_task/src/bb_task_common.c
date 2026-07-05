// bb_task — pure resolver, base registry ops, and mark-and-sweep evaluator.
// Portable: no FreeRTOS types. Compiled on both host (tests) and ESP-IDF as
// part of the bb_task component. This file is the coverage-GATED SSOT --
// every branch here must be host-tested (see components/bb_task/include/
// bb_task.h and test/test_host/test_bb_task.c).

#include "bb_task.h"
#include "bb_registry.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>

// Kconfig bridge (see CLAUDE.md "Avoiding audit-class regressions").
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_TASK_BASE_MAX
#define BB_TASK_BASE_MAX CONFIG_BB_TASK_BASE_MAX
#endif
#ifndef BB_TASK_BASE_MAX
#define BB_TASK_BASE_MAX 24
#endif

// ---------------------------------------------------------------------------
// bb_task_resolve — pure, no FreeRTOS types.
// ---------------------------------------------------------------------------

bb_err_t bb_task_resolve(const bb_task_config_t *cfg, int num_cores,
                          bb_task_resolved_t *out)
{
    if (!cfg || !out) {
        return BB_ERR_INVALID_ARG;
    }
    if (!cfg->entry) {
        return BB_ERR_INVALID_ARG;
    }
    if (cfg->stack_bytes == 0) {
        return BB_ERR_INVALID_ARG;
    }
    if (cfg->backing == BB_TASK_BACKING_STATIC) {
        if (!cfg->stack_buf || !cfg->tcb_buf) {
            return BB_ERR_INVALID_ARG;
        }
    }

    // Unicore clamp: mirrors the hand-rolled
    // `if (core != tskNO_AFFINITY && core >= configNUMBER_OF_CORES) core =
    // tskNO_AFFINITY;` idiom duplicated across bb_ota_pull/bb_ota_check/
    // bb_task_registry_sw_wdt -- owned once, here.
    int core = cfg->core;
    if (core != BB_TASK_CORE_ANY && core >= num_cores) {
        core = BB_TASK_CORE_ANY;
    }

    out->core         = core;
    out->stack_bytes  = cfg->stack_bytes;
    out->backing      = cfg->backing;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Base registry — ptr-keyed (opaque task handle), payload pool mirrors the
// bb_task_registry s_pool idiom (see platform/host/bb_task_registry/
// bb_task_registry.c for the fuller precedent this borrows from).
// ---------------------------------------------------------------------------

static bb_task_base_entry_t s_pool[BB_TASK_BASE_MAX];
static pthread_mutex_t      s_lock = PTHREAD_MUTEX_INITIALIZER;

BB_REGISTRY_DEFINE_TAGGED(s_base_registry, BB_TASK_BASE_MAX, "task_base");

static int pool_alloc_locked(void)
{
    for (int i = 0; i < BB_TASK_BASE_MAX; i++) {
        if (!s_pool[i].in_use) {
            s_pool[i].in_use = true;
            return i;
        }
    }
    return -1;
}

// All call sites pass an entry obtained from pool_alloc_locked (upsert's
// new-handle path) or a non-NULL registry lookup (remove) -- entry is never
// NULL here, so this is an unconditional clear rather than a guarded one
// (avoids an untestable dead branch, mirrors bb_task_registry).
static void pool_free_locked(bb_task_base_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));
}

static void copy_name(bb_task_base_entry_t *entry, const char *name)
{
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
}

bb_err_t bb_task_base_upsert(void *handle, const char *name,
                              uint32_t stack_bytes, bool wdt_arm)
{
    if (!handle || !name) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&s_lock);

    void *existing = bb_registry_lookup_ptr(&s_base_registry, handle);
    if (existing) {
        // Update-if-present: tolerate re-invocation on the same handle
        // (pool-recycle reuse) -- never double-inserts.
        bb_task_base_entry_t *entry = (bb_task_base_entry_t *)existing;
        copy_name(entry, name);
        entry->stack_bytes = stack_bytes;
        entry->wdt_arm     = wdt_arm;
        pthread_mutex_unlock(&s_lock);
        return BB_OK;
    }

    int idx = pool_alloc_locked();
    if (idx < 0) {
        pthread_mutex_unlock(&s_lock);
        return BB_ERR_NO_SPACE;
    }

    bb_task_base_entry_t *entry = &s_pool[idx];
    entry->handle       = handle;
    copy_name(entry, name);
    entry->stack_bytes  = stack_bytes;
    entry->wdt_arm      = wdt_arm;
    entry->seen_tick    = 0;

    // pool_alloc_locked found a free pool slot, this component's pool
    // occupancy is kept in exact 1:1 sync with the registry's key count (a
    // pool slot is only ever occupied by this exact call, and only ever
    // freed by bb_task_base_remove alongside the matching deregister_ptr),
    // and `handle` was confirmed absent from the registry under this same
    // lock above -- so bb_registry_register_ptr cannot fail here (neither
    // BB_ERR_NO_SPACE nor a duplicate-key BB_ERR_INVALID_STATE is
    // reachable). Register unconditionally rather than gate on an
    // unreachable (and therefore untestable) error branch (mirrors
    // bb_task_base_remove's identical rationale below).
    bb_registry_register_ptr(&s_base_registry, handle, entry);

    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_task_base_remove(void *handle)
{
    if (!handle) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&s_lock);

    void *value = bb_registry_lookup_ptr(&s_base_registry, handle);
    if (!value) {
        pthread_mutex_unlock(&s_lock);
        return BB_ERR_NOT_FOUND;
    }

    // value was resolved under this same lock and s_base_registry is never
    // frozen, so deregister_ptr cannot fail here -- free unconditionally
    // rather than gate on an unreachable (and therefore untestable) error
    // branch (mirrors bb_task_registry_deregister).
    bb_registry_deregister_ptr(&s_base_registry, handle);
    pool_free_locked((bb_task_base_entry_t *)value);

    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

// Trampoline: bridges bb_registry_foreach_ptr's (void*key, void*value, ctx)
// shape to bb_task_base_cb_t's (handle, const entry*, ctx) shape.
typedef struct {
    bb_task_base_cb_t cb;
    void              *ctx;
} foreach_bridge_t;

static void foreach_trampoline(void *key, void *value, void *ctx)
{
    foreach_bridge_t *bridge = (foreach_bridge_t *)ctx;
    bridge->cb(key, (const bb_task_base_entry_t *)value, bridge->ctx);
}

void bb_task_base_foreach(bb_task_base_cb_t cb, void *ctx)
{
    if (!cb) {
        return;
    }
    // Wrapper lock held across the entire call (mirrors
    // bb_task_registry_foreach): bb_registry_foreach_ptr's own internal
    // lock only protects the registry's name/value table, not the payload
    // pool (s_pool) this component owns on top of it.
    pthread_mutex_lock(&s_lock);
    foreach_bridge_t bridge = { .cb = cb, .ctx = ctx };
    bb_registry_foreach_ptr(&s_base_registry, foreach_trampoline, &bridge);
    pthread_mutex_unlock(&s_lock);
}

// ---------------------------------------------------------------------------
// bb_task_base_sweep_apply — pure mark-and-sweep evaluator over a
// caller-supplied array. See bb_health_stack_table_sweep for the identical
// grace-window rationale this mirrors.
// ---------------------------------------------------------------------------

#define BB_TASK_SWEEP_GRACE 1

int bb_task_base_sweep_apply(bb_task_base_entry_t *entries, int n, uint32_t now_tick)
{
    if (!entries || n <= 0) {
        return 0;
    }

    int freed = 0;
    for (int i = 0; i < n; i++) {
        if (!entries[i].in_use) {
            continue;
        }
        if (entries[i].seen_tick == now_tick) {
            continue;  // seen this scan -- alive, leave untouched
        }
        if ((now_tick - entries[i].seen_tick) > BB_TASK_SWEEP_GRACE) {
            memset(&entries[i], 0, sizeof(entries[i]));
            freed++;
        }
        // else: missed exactly one scan -- within grace, left untouched.
    }
    return freed;
}

// ---------------------------------------------------------------------------
// bb_task_deregister — thin portable wrapper over bb_task_base_remove.
// ---------------------------------------------------------------------------

bb_err_t bb_task_deregister(void *handle)
{
    return bb_task_base_remove(handle);
}

#ifdef BB_TASK_TESTING
void bb_task_base_test_reset(void)
{
    pthread_mutex_lock(&s_lock);
    bb_registry_reset(&s_base_registry);
    memset(s_pool, 0, sizeof(s_pool));
    pthread_mutex_unlock(&s_lock);
}
#endif
