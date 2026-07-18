// bb_collection — humble ordered collection of caller-owned opaque items.
//
// Compiled on both host (tests) and ESP-IDF (via bb_collection CMakeLists
// SRCS). All operations are guarded by a per-collection bb_lock_t (bb_core)
// — its host backend wraps pthread_mutex_t, its ESP-IDF backend a FreeRTOS
// semaphore; this file never sees either platform type directly. foreach
// uses a fixed-size stack copy-out so the callback runs without the lock
// held — same shape as bb_registry_foreach.

#include "bb_collection.h"
#include "bb_log.h"

#include <string.h>

static const char *TAG = "bb_collection";

// Lazily bb_lock_init() c->lock exactly once (bb_once_run) — mirrors
// BB_COLLECTION_DEFINE's "no explicit init call needed" contract while
// keeping the platform mutex type out of bb_collection.h.
static void collection_init_lock(void *ctx)
{
    bb_collection_t *c = ctx;
    bb_lock_config_t cfg = { .name = "bb_collection" };
    bb_lock_init(&cfg, &c->lock);
}

static inline void collection_ensure_lock(bb_collection_t *c)
{
    bb_once_run(&c->lock_once, collection_init_lock, c);
}

bb_err_t bb_collection_add(bb_collection_t *c, const char *name,
                            const void *item, int order)
{
    if (c == NULL || name == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    collection_ensure_lock(c);
    bb_lock_lock(&c->lock);

    if (c->count >= c->capacity) {
        bb_log_e(TAG, "add('%s'): collection full (cap %zu)", name, c->capacity);
        bb_lock_unlock(&c->lock);
        return BB_ERR_NO_SPACE;
    }

    c->entries[c->count].name  = name;
    c->entries[c->count].item  = item;
    c->entries[c->count].order = order;
    c->count++;

    bb_lock_unlock(&c->lock);
    return BB_OK;
}

// Snapshot entries[] into a fixed-size stack buffer under the lock, releasing
// it before returning.
static size_t collection_snapshot(bb_collection_t *c,
                                   bb_collection_entry_t snapshot[BB_COLLECTION_SNAPSHOT_MAX])
{
    collection_ensure_lock(c);
    bb_lock_lock(&c->lock);
    size_t count = c->count;

    // Fixed-size stack buffer bounded by BB_COLLECTION_SNAPSHOT_MAX.
    // _Static_assert in BB_COLLECTION_DEFINE enforces capacity <=
    // SNAPSHOT_MAX, so count <= capacity <= BB_COLLECTION_SNAPSHOT_MAX
    // always holds.
    for (size_t i = 0; i < count; i++) {
        snapshot[i] = c->entries[i];
    }
    bb_lock_unlock(&c->lock);
    return count;
}

// Stable insertion sort by ascending `order`. count is bounded by
// BB_COLLECTION_SNAPSHOT_MAX (small, O(tens)); insertion sort keeps this
// allocation-free and trivially stable without extra bookkeeping.
static void collection_stable_sort(bb_collection_entry_t *snapshot, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        bb_collection_entry_t key = snapshot[i];
        size_t j = i;
        while (j > 0 && snapshot[j - 1].order > key.order) {
            snapshot[j] = snapshot[j - 1];
            j--;
        }
        snapshot[j] = key;
    }
}

void bb_collection_foreach(bb_collection_t *c, bb_collection_cb_t cb, void *ctx)
{
    if (c == NULL || cb == NULL) {
        return;
    }

    bb_collection_entry_t snapshot[BB_COLLECTION_SNAPSHOT_MAX];
    size_t count = collection_snapshot(c, snapshot);
    if (count == 0) {
        return;
    }

    collection_stable_sort(snapshot, count);

    for (size_t i = 0; i < count; i++) {
        cb(&snapshot[i], ctx);
    }
}

size_t bb_collection_count(bb_collection_t *c)
{
    if (c == NULL) {
        return 0;
    }

    collection_ensure_lock(c);
    bb_lock_lock(&c->lock);
    size_t count = c->count;
    bb_lock_unlock(&c->lock);
    return count;
}

#ifdef BB_COLLECTION_TESTING
void bb_collection_reset(bb_collection_t *c)
{
    bb_lock_destroy(&c->lock);
    c->count     = 0;
    c->lock_once = (bb_once_t)BB_ONCE_INIT;
}
#endif
