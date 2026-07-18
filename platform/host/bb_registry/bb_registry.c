// bb_registry — generic name→handle object registry.
//
// Compiled on both host (tests) and ESP-IDF (via bb_registry CMakeLists SRCS).
// All operations are guarded by a per-registry bb_lock_t (bb_core) — its
// host backend wraps pthread_mutex_t, its ESP-IDF backend a FreeRTOS
// semaphore; this file never sees either platform type directly.
// foreach uses a stack copy-out so callbacks run without the lock held.

#include "bb_registry.h"
#include "bb_log.h"

#include <inttypes.h>
#include <string.h>
#include <stdbool.h>

static const char *s_default_tag = "bb_registry";

// Resolve the effective tag for a registry — fall back to default when unset.
static inline const char *s_tag(const bb_registry_t *r)
{
    return r->tag ? r->tag : s_default_tag;
}

// Lazily bb_lock_init() r->lock exactly once (bb_once_run) — mirrors
// BB_REGISTRY_DEFINE's "no explicit init call needed" contract while keeping
// the platform mutex type out of bb_registry.h.
static void registry_init_lock(void *ctx)
{
    bb_registry_t *r = ctx;
    bb_lock_config_t cfg = { .name = "bb_registry", .category = r->tag };
    bb_lock_init(&cfg, &r->lock);
}

static inline void registry_ensure_lock(bb_registry_t *r)
{
    bb_once_run(&r->lock_once, registry_init_lock, r);
}

// Scan entries[] for the slot matching key. by_ptr selects the comparison:
// false -> strcmp(entries[i].name, key), true -> entries[i].name == key.
// Caller holds r->lock. Returns the index or -1 if not found.
static int registry_find_index(const bb_registry_t *r, const void *key, bool by_ptr)
{
    for (uint16_t i = 0; i < r->count; i++) {
        if (by_ptr) {
            if ((const void *)r->entries[i].name == key) {
                return (int)i;
            }
        } else {
            if (strcmp(r->entries[i].name, (const char *)key) == 0) {
                return (int)i;
            }
        }
    }
    return -1;
}

// Shared register path for both name-keyed and pointer-keyed variants.
// Caller holds no lock; key/value have already been checked non-NULL.
// log_prefix names the caller for consistent log messages ("register" or
// "register_ptr").
static bb_err_t registry_register_common(bb_registry_t *r, const void *key,
                                          void *value, bool by_ptr,
                                          const char *log_prefix)
{
    registry_ensure_lock(r);
    bb_lock_lock(&r->lock);

    if (r->frozen) {
        if (by_ptr) {
            bb_log_w(s_tag(r), "%s(%p): registry is frozen", log_prefix, key);
        } else {
            bb_log_w(s_tag(r), "%s('%s'): registry is frozen", log_prefix,
                     (const char *)key);
        }
        bb_lock_unlock(&r->lock);
        return BB_ERR_INVALID_STATE;
    }

    if (registry_find_index(r, key, by_ptr) >= 0) {
        if (by_ptr) {
            bb_log_w(s_tag(r), "%s(%p): duplicate key, ignored", log_prefix, key);
        } else {
            bb_log_w(s_tag(r), "%s('%s'): duplicate name, ignored", log_prefix,
                     (const char *)key);
        }
        bb_lock_unlock(&r->lock);
        return BB_ERR_INVALID_STATE;
    }

    if (r->count >= r->capacity) {
        if (by_ptr) {
            bb_log_w(s_tag(r), "%s(%p): registry full (cap %" PRIu16 ")",
                     log_prefix, key, r->capacity);
        } else {
            bb_log_w(s_tag(r), "%s('%s'): registry full (cap %" PRIu16 ")",
                     log_prefix, (const char *)key, r->capacity);
        }
        bb_lock_unlock(&r->lock);
        return BB_ERR_NO_SPACE;
    }

    r->entries[r->count].name  = (const char *)key;
    r->entries[r->count].value = value;
    r->count++;

    // HWM warning — fire once when count transitions to capacity-1
    // (one slot still free), giving callers notice before the registry fills.
    if (!r->hwm_warned && r->count == (uint16_t)(r->capacity - 1u)) {
        bb_log_w(s_tag(r), "high-watermark: count %" PRIu16 " / %" PRIu16,
                 r->count, r->capacity);
        r->hwm_warned = true;
    }

    bb_lock_unlock(&r->lock);
    return BB_OK;
}

// Shared deregister path for both name-keyed and pointer-keyed variants.
// Caller holds no lock; key has already been checked non-NULL.
static bb_err_t registry_deregister_common(bb_registry_t *r, const void *key,
                                            bool by_ptr, const char *log_prefix)
{
    registry_ensure_lock(r);
    bb_lock_lock(&r->lock);

    if (r->frozen) {
        if (by_ptr) {
            bb_log_w(s_tag(r), "%s(%p): registry is frozen", log_prefix, key);
        } else {
            bb_log_w(s_tag(r), "%s('%s'): registry is frozen", log_prefix,
                     (const char *)key);
        }
        bb_lock_unlock(&r->lock);
        return BB_ERR_INVALID_STATE;
    }

    int idx = registry_find_index(r, key, by_ptr);
    if (idx < 0) {
        bb_lock_unlock(&r->lock);
        return BB_ERR_NOT_FOUND;
    }

    // Compact: shift entries left over the removed slot.
    uint16_t tail = (uint16_t)(r->count - 1u);
    for (uint16_t j = (uint16_t)idx; j < tail; j++) {
        r->entries[j] = r->entries[j + 1];
    }
    r->count = tail;

    bb_lock_unlock(&r->lock);
    return BB_OK;
}

// Shared lookup path for both name-keyed and pointer-keyed variants.
// Caller holds no lock; key has already been checked non-NULL.
static void *registry_lookup_common(bb_registry_t *r, const void *key, bool by_ptr)
{
    registry_ensure_lock(r);
    bb_lock_lock(&r->lock);
    int idx = registry_find_index(r, key, by_ptr);
    void *v = (idx >= 0) ? r->entries[idx].value : NULL;
    bb_lock_unlock(&r->lock);
    return v;
}

// ---------------------------------------------------------------------------
// register
// ---------------------------------------------------------------------------

bb_err_t bb_registry_register(bb_registry_t *r, const char *name, void *value)
{
    if (!name || !value) {
        return BB_ERR_INVALID_ARG;
    }
    return registry_register_common(r, name, value, false, "register");
}

// ---------------------------------------------------------------------------
// deregister
// ---------------------------------------------------------------------------

bb_err_t bb_registry_deregister(bb_registry_t *r, const char *name)
{
    if (!name) {
        return BB_ERR_INVALID_ARG;
    }
    return registry_deregister_common(r, name, false, "deregister");
}

// ---------------------------------------------------------------------------
// freeze
// ---------------------------------------------------------------------------

void bb_registry_freeze(bb_registry_t *r)
{
    registry_ensure_lock(r);
    bb_lock_lock(&r->lock);
    r->frozen = true;
    bb_lock_unlock(&r->lock);
}

// ---------------------------------------------------------------------------
// foreach — copy-out pattern; callbacks run without the lock held
// ---------------------------------------------------------------------------

// Snapshot entries[] into a fixed-size stack buffer under the lock, releasing
// it before returning. Shared by bb_registry_foreach and _foreach_ptr.
static uint16_t registry_snapshot(bb_registry_t *r,
                                   bb_registry_entry_t snapshot[BB_REGISTRY_SNAPSHOT_MAX])
{
    registry_ensure_lock(r);
    bb_lock_lock(&r->lock);
    uint16_t count = r->count;

    // Fixed-size stack buffer bounded by BB_REGISTRY_SNAPSHOT_MAX.
    // _Static_assert in BB_REGISTRY_DEFINE enforces capacity <= SNAPSHOT_MAX,
    // so count <= capacity <= BB_REGISTRY_SNAPSHOT_MAX is always true.
    for (uint16_t i = 0; i < count; i++) {
        snapshot[i] = r->entries[i];
    }
    bb_lock_unlock(&r->lock);
    return count;
}

void bb_registry_foreach(bb_registry_t *r,
                         void (*cb)(const char *name, void *value, void *ctx),
                         void *ctx)
{
    if (!cb) {
        return;
    }

    bb_registry_entry_t snapshot[BB_REGISTRY_SNAPSHOT_MAX];
    uint16_t count = registry_snapshot(r, snapshot);

    for (uint16_t i = 0; i < count; i++) {
        cb(snapshot[i].name, snapshot[i].value, ctx);
    }
}

void bb_registry_foreach_ptr(bb_registry_t *r,
                             void (*cb)(void *key, void *value, void *ctx),
                             void *ctx)
{
    if (!cb) {
        return;
    }

    bb_registry_entry_t snapshot[BB_REGISTRY_SNAPSHOT_MAX];
    uint16_t count = registry_snapshot(r, snapshot);

    for (uint16_t i = 0; i < count; i++) {
        cb((void *)snapshot[i].name, snapshot[i].value, ctx);
    }
}

// ---------------------------------------------------------------------------
// count
// ---------------------------------------------------------------------------

uint16_t bb_registry_count(bb_registry_t *r)
{
    registry_ensure_lock(r);
    bb_lock_lock(&r->lock);
    uint16_t c = r->count;
    bb_lock_unlock(&r->lock);
    return c;
}

// ---------------------------------------------------------------------------
// get_by_index
// ---------------------------------------------------------------------------

bb_err_t bb_registry_get_by_index(bb_registry_t *r, uint16_t idx,
                                   bb_registry_entry_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }

    registry_ensure_lock(r);
    bb_lock_lock(&r->lock);
    if (idx >= r->count) {
        bb_lock_unlock(&r->lock);
        return BB_ERR_NOT_FOUND;
    }
    *out = r->entries[idx];
    bb_lock_unlock(&r->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// lookup
// ---------------------------------------------------------------------------

void *bb_registry_lookup(bb_registry_t *r, const char *name)
{
    if (!name) {
        return NULL;
    }
    return registry_lookup_common(r, name, false);
}

// ---------------------------------------------------------------------------
// register_ptr — pointer-keyed variant; .name reinterpreted as identity key
// ---------------------------------------------------------------------------

bb_err_t bb_registry_register_ptr(bb_registry_t *r, void *key, void *value)
{
    if (!key || !value) {
        return BB_ERR_INVALID_ARG;
    }
    return registry_register_common(r, key, value, true, "register_ptr");
}

// ---------------------------------------------------------------------------
// deregister_ptr
// ---------------------------------------------------------------------------

bb_err_t bb_registry_deregister_ptr(bb_registry_t *r, const void *key)
{
    if (!key) {
        return BB_ERR_INVALID_ARG;
    }
    return registry_deregister_common(r, key, true, "deregister_ptr");
}

// ---------------------------------------------------------------------------
// lookup_ptr
// ---------------------------------------------------------------------------

void *bb_registry_lookup_ptr(bb_registry_t *r, const void *key)
{
    if (!key) {
        return NULL;
    }
    return registry_lookup_common(r, key, true);
}

// ---------------------------------------------------------------------------
// Test reset (BB_REGISTRY_TESTING only)
// ---------------------------------------------------------------------------

#ifdef BB_REGISTRY_TESTING
void bb_registry_reset(bb_registry_t *r)
{
    bb_lock_destroy(&r->lock);
    r->count      = 0;
    r->frozen     = false;
    r->hwm_warned = false;
    r->lock_once  = (bb_once_t)BB_ONCE_INIT;
}
#endif
