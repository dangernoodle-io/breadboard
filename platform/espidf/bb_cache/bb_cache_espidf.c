// Must come before any system header on Linux — glibc gates PTHREAD_MUTEX_RECURSIVE
// on _GNU_SOURCE (or _XOPEN_SOURCE >= 500). macOS exposes it unconditionally.
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "bb_cache.h"
#include "bb_event.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_core.h"

#include <string.h>
#include <stdlib.h>
#include <pthread.h>

static const char *TAG = "bb_cache";

// ---------------------------------------------------------------------------
// Registry entry
// ---------------------------------------------------------------------------

typedef struct {
    const char          *topic;           // NULL = slot free
    const void         *(*snapshot)(void); // NULL = owned mode
    void                *owned;           // heap buffer in owned mode; NULL in getter mode
    size_t               size;            // sizeof owned struct (owned mode only)
    bb_cache_serialize_fn fn;
    bb_event_topic_t     event_topic;     // registered event topic handle
    pthread_mutex_t      lock;
} bb_cache_entry_t;

static bb_cache_entry_t s_entries[BB_CACHE_MAX_TOPICS];
static pthread_mutex_t  s_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static bool             s_initialized = false;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void ensure_init(void)
{
    if (s_initialized) return;
    pthread_mutex_lock(&s_reg_lock);
    if (!s_initialized) {
        for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
            s_entries[i].topic = NULL;
        }
        s_initialized = true;
    }
    pthread_mutex_unlock(&s_reg_lock);
}

static bb_cache_entry_t *find_entry(const char *topic)
{
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].topic && strcmp(s_entries[i].topic, topic) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

// Serialize entry contents into obj under the entry's lock.
// Entry must not be NULL. Caller holds no lock before calling.
static bb_err_t serialize_locked(bb_cache_entry_t *e, bb_json_t obj)
{
    pthread_mutex_lock(&e->lock);

    const void *snap;
    if (e->snapshot) {
        snap = e->snapshot();
    } else {
        snap = e->owned;
    }

    if (!snap) {
        pthread_mutex_unlock(&e->lock);
        bb_log_w(TAG, "topic '%s': no snapshot available", e->topic);
        return BB_ERR_INVALID_STATE;
    }

    e->fn(obj, snap);
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_cache_register(const char *topic,
                           const void *(*snapshot)(void),
                           size_t snap_size,
                           bb_cache_serialize_fn serialize)
{
    if (!topic || !serialize) return BB_ERR_INVALID_ARG;

    ensure_init();

    pthread_mutex_lock(&s_reg_lock);

    // Idempotent: already registered?
    if (find_entry(topic)) {
        pthread_mutex_unlock(&s_reg_lock);
        return BB_OK;
    }

    // Find a free slot
    bb_cache_entry_t *slot = NULL;
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (!s_entries[i].topic) {
            slot = &s_entries[i];
            break;
        }
    }

    if (!slot) {
        pthread_mutex_unlock(&s_reg_lock);
        bb_log_e(TAG, "registry full (max %d)", BB_CACHE_MAX_TOPICS);
        return BB_ERR_NO_SPACE;
    }

    // Owned mode: allocate struct buffer
    void *owned = NULL;
    if (!snapshot) {
        if (snap_size == 0) {
            pthread_mutex_unlock(&s_reg_lock);
            return BB_ERR_INVALID_ARG;
        }
        owned = calloc(1, snap_size);
        if (!owned) {
            pthread_mutex_unlock(&s_reg_lock);
            return BB_ERR_NO_SPACE;
        }
    }

    // Register event topic (best-effort — may not be initialized on host/test).
    bb_event_topic_t ev_topic = NULL;
    bb_event_topic_register(topic, &ev_topic);

    // Init per-entry mutex (recursive so serialize_locked can be called
    // from within a locked section without deadlock in future use)
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&slot->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    slot->topic       = topic;
    slot->snapshot    = snapshot;
    slot->owned       = owned;
    slot->size        = snap_size;
    slot->fn          = serialize;
    slot->event_topic = ev_topic;

    pthread_mutex_unlock(&s_reg_lock);
    return BB_OK;
}

bb_err_t bb_cache_update(const char *topic, const void *snap)
{
    if (!topic || !snap) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry(topic);
    if (!e) return BB_ERR_NOT_FOUND;

    // Getter-mode: caller owns the struct; update is a no-op
    if (e->snapshot) return BB_OK;

    pthread_mutex_lock(&e->lock);
    memcpy(e->owned, snap, e->size);
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

bb_err_t bb_cache_post(const char *topic)
{
    if (!topic) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry(topic);
    if (!e) return BB_ERR_NOT_FOUND;
    if (!e->event_topic) return BB_ERR_INVALID_STATE;

    bb_json_t obj = bb_json_obj_new();
    if (!obj) return BB_ERR_NO_SPACE;

    bb_err_t err = serialize_locked(e, obj);
    if (err != BB_OK) {
        bb_json_free(obj);
        return err;
    }

    char *payload = bb_json_serialize(obj);
    bb_json_free(obj);
    if (!payload) return BB_ERR_NO_SPACE;

    size_t len = strlen(payload);
    err = bb_event_post(e->event_topic, 0, payload, len + 1);
    bb_json_free_str(payload);
    return err;
}

bb_err_t bb_cache_serialize_into(const char *topic, bb_json_t obj)
{
    if (!topic || !obj) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry(topic);
    if (!e) return BB_ERR_NOT_FOUND;

    return serialize_locked(e, obj);
}

// ---------------------------------------------------------------------------
// Test reset (guarded by BB_CACHE_TESTING)
// ---------------------------------------------------------------------------

#ifdef BB_CACHE_TESTING
void bb_cache_reset_for_test(void)
{
    pthread_mutex_lock(&s_reg_lock);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].topic) {
            pthread_mutex_destroy(&s_entries[i].lock);
            free(s_entries[i].owned);
            s_entries[i].topic    = NULL;
            s_entries[i].owned    = NULL;
            s_entries[i].snapshot = NULL;
            s_entries[i].fn       = NULL;
            s_entries[i].event_topic = NULL;
        }
    }
    s_initialized = false;
    pthread_mutex_unlock(&s_reg_lock);
}
#endif
