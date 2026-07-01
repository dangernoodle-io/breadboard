// bb_task_registry — thin consumer of the generic bb_registry primitive.
// Compiled on both host (tests) and ESP-IDF as part of the bb_task_registry
// component.

#include "bb_task_registry.h"
#include "bb_registry.h"
#include "bb_log.h"
#include "bb_clock.h"
#include "bb_wdt.h"

#include <pthread.h>
#include <stdatomic.h>
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
} bb_task_entry_t;

static bb_task_entry_t s_pool[BB_TASK_REGISTRY_MAX];

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
    } else if (out_token) {
        out_token->index      = (uint16_t)idx;
        out_token->generation = generation;
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
#endif
