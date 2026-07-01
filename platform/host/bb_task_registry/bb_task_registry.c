// bb_task_registry — thin consumer of the generic bb_registry primitive.
// Compiled on both host (tests) and ESP-IDF as part of the bb_task_registry
// component.

#include "bb_task_registry.h"
#include "bb_registry.h"
#include "bb_log.h"

#include <pthread.h>
#include <string.h>

/* Kconfig bridge: honour CONFIG_BB_TASK_REGISTRY_MAX from build flags; default 24. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_task_wdt.h"
#endif
#ifdef CONFIG_BB_TASK_REGISTRY_MAX
#define BB_TASK_REGISTRY_MAX CONFIG_BB_TASK_REGISTRY_MAX
#endif
#ifndef BB_TASK_REGISTRY_MAX
#define BB_TASK_REGISTRY_MAX 24
#endif

static const char *TAG = "bb_task_registry";

// Payload pool — bb_registry stores name->void*; this pool backs the void*
// values with the extra fields (stack budget, wdt state, handle) this
// component needs. One slot per registry capacity slot.
typedef struct {
    void    *handle;
    uint32_t stack_budget_bytes;
    bool     wdt_subscribed;
    bool     in_use;
} bb_task_entry_t;

static bb_task_entry_t s_pool[BB_TASK_REGISTRY_MAX];

BB_REGISTRY_DEFINE_TAGGED(s_task_registry, BB_TASK_REGISTRY_MAX, "tasks");

// Wrapper mutex — serialises the public ops as a single atomic unit over the
// top of the primitive's own internal lock, and also protects the payload
// pool (s_pool) which the primitive knows nothing about. Mirrors
// bb_ring_registry's s_ring_reg_lock pattern (see that file for the full
// TOCTOU/UAF rationale) — always taken OUTSIDE any bb_registry_* call, so
// there is no lock-order inversion.
static pthread_mutex_t s_task_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static int pool_alloc_locked(void)
{
    for (int i = 0; i < BB_TASK_REGISTRY_MAX; i++) {
        if (!s_pool[i].in_use) {
            s_pool[i].in_use = true;
            return i;
        }
    }
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

// Best-effort, observational query of whether `handle` is currently
// subscribed to the ESP-IDF Task WDT. Read-only — never subscribes or
// unsubscribes anything (auto-subscription is deferred to B1-458). On host,
// or when handle is NULL, always false.
#if defined(ESP_PLATFORM)
static bool query_wdt_subscribed(void *handle)
{
    if (!handle) {
        return false;
    }
    return esp_task_wdt_status((TaskHandle_t)handle) == ESP_OK;
}
#else
static bool query_wdt_subscribed(void *handle)
{
    (void)handle;
    return false;
}
#endif

bb_err_t bb_task_registry_register(const char *name, uint32_t stack_budget_bytes, void *handle)
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
    entry->wdt_subscribed      = query_wdt_subscribed(handle);

    bb_err_t err = bb_registry_register(&s_task_registry, name, entry);
    if (err != BB_OK) {
        pool_free_locked(entry);
    }

    pthread_mutex_unlock(&s_task_reg_lock);

    if (err != BB_OK) {
        bb_log_w(TAG, "register('%s') failed: %d", name, (int)err);
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
        pool_free_locked(scan.found_entry);
    }

    pthread_mutex_unlock(&s_task_reg_lock);
    return err;
}

uint16_t bb_task_registry_count(void)
{
    pthread_mutex_lock(&s_task_reg_lock);
    uint16_t count = bb_registry_count(&s_task_registry);
    pthread_mutex_unlock(&s_task_reg_lock);
    return count;
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
    pthread_mutex_unlock(&s_task_reg_lock);
}

bb_err_t bb_task_registry_test_seed(const char *name, uint32_t stack_budget_bytes, bool wdt_subscribed)
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

    bb_err_t err = bb_registry_register(&s_task_registry, name, entry);
    if (err != BB_OK) {
        pool_free_locked(entry);
    }
    pthread_mutex_unlock(&s_task_reg_lock);
    return err;
}
#endif
