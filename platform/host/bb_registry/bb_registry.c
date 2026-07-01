// bb_registry — generic name→handle object registry.
//
// Compiled on both host (tests) and ESP-IDF (via bb_registry CMakeLists SRCS).
// All operations are guarded by a per-registry pthread_mutex_t.
// foreach uses a stack copy-out so callbacks run without the lock held.

#include "bb_registry.h"
#include "bb_log.h"

#include <inttypes.h>
#include <string.h>
#include <pthread.h>

static const char *s_default_tag = "bb_registry";

// Resolve the effective tag for a registry — fall back to default when unset.
static inline const char *s_tag(const bb_registry_t *r)
{
    return r->tag ? r->tag : s_default_tag;
}

// ---------------------------------------------------------------------------
// register
// ---------------------------------------------------------------------------

bb_err_t bb_registry_register(bb_registry_t *r, const char *name, void *value)
{
    if (!name || !value) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&r->lock);

    if (r->frozen) {
        bb_log_w(s_tag(r), "register('%s'): registry is frozen", name);
        pthread_mutex_unlock(&r->lock);
        return BB_ERR_INVALID_STATE;
    }

    // Duplicate-name check.
    for (uint16_t i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].name, name) == 0) {
            bb_log_w(s_tag(r), "register('%s'): duplicate name, ignored", name);
            pthread_mutex_unlock(&r->lock);
            return BB_ERR_INVALID_STATE;
        }
    }

    if (r->count >= r->capacity) {
        bb_log_w(s_tag(r), "register('%s'): registry full (cap %" PRIu16 ")",
                 name, r->capacity);
        pthread_mutex_unlock(&r->lock);
        return BB_ERR_NO_SPACE;
    }

    r->entries[r->count].name  = name;
    r->entries[r->count].value = value;
    r->count++;

    // HWM warning — fire once when count transitions to capacity-1
    // (one slot still free), giving callers notice before the registry fills.
    if (!r->hwm_warned && r->count == (uint16_t)(r->capacity - 1u)) {
        bb_log_w(s_tag(r), "high-watermark: count %" PRIu16 " / %" PRIu16,
                 r->count, r->capacity);
        r->hwm_warned = true;
    }

    pthread_mutex_unlock(&r->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// deregister
// ---------------------------------------------------------------------------

bb_err_t bb_registry_deregister(bb_registry_t *r, const char *name)
{
    if (!name) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&r->lock);

    if (r->frozen) {
        bb_log_w(s_tag(r), "deregister('%s'): registry is frozen", name);
        pthread_mutex_unlock(&r->lock);
        return BB_ERR_INVALID_STATE;
    }

    for (uint16_t i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].name, name) == 0) {
            // Compact: shift entries left over the removed slot.
            uint16_t tail = (uint16_t)(r->count - 1u);
            for (uint16_t j = i; j < tail; j++) {
                r->entries[j] = r->entries[j + 1];
            }
            r->count = tail;
            pthread_mutex_unlock(&r->lock);
            return BB_OK;
        }
    }

    pthread_mutex_unlock(&r->lock);
    return BB_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// freeze
// ---------------------------------------------------------------------------

void bb_registry_freeze(bb_registry_t *r)
{
    pthread_mutex_lock(&r->lock);
    r->frozen = true;
    pthread_mutex_unlock(&r->lock);
}

// ---------------------------------------------------------------------------
// foreach — copy-out pattern; callbacks run without the lock held
// ---------------------------------------------------------------------------

void bb_registry_foreach(bb_registry_t *r,
                         void (*cb)(const char *name, void *value, void *ctx),
                         void *ctx)
{
    if (!cb) {
        return;
    }

    pthread_mutex_lock(&r->lock);
    uint16_t count = r->count;

    // Fixed-size stack buffer bounded by BB_REGISTRY_SNAPSHOT_MAX.
    // _Static_assert in BB_REGISTRY_DEFINE enforces capacity <= SNAPSHOT_MAX,
    // so count <= capacity <= BB_REGISTRY_SNAPSHOT_MAX is always true.
    bb_registry_entry_t snapshot[BB_REGISTRY_SNAPSHOT_MAX];
    for (uint16_t i = 0; i < count; i++) {
        snapshot[i] = r->entries[i];
    }
    pthread_mutex_unlock(&r->lock);

    for (uint16_t i = 0; i < count; i++) {
        cb(snapshot[i].name, snapshot[i].value, ctx);
    }
}

// ---------------------------------------------------------------------------
// count
// ---------------------------------------------------------------------------

uint16_t bb_registry_count(bb_registry_t *r)
{
    pthread_mutex_lock(&r->lock);
    uint16_t c = r->count;
    pthread_mutex_unlock(&r->lock);
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

    pthread_mutex_lock(&r->lock);
    if (idx >= r->count) {
        pthread_mutex_unlock(&r->lock);
        return BB_ERR_NOT_FOUND;
    }
    *out = r->entries[idx];
    pthread_mutex_unlock(&r->lock);
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

    pthread_mutex_lock(&r->lock);
    for (uint16_t i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].name, name) == 0) {
            void *v = r->entries[i].value;
            pthread_mutex_unlock(&r->lock);
            return v;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

// ---------------------------------------------------------------------------
// Test reset (BB_REGISTRY_TESTING only)
// ---------------------------------------------------------------------------

#ifdef BB_REGISTRY_TESTING
void bb_registry_reset(bb_registry_t *r)
{
    pthread_mutex_destroy(&r->lock);
    r->count      = 0;
    r->frozen     = false;
    r->hwm_warned = false;
    pthread_mutex_init(&r->lock, NULL);
}
#endif
