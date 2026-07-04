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
#include "bb_mem.h"
#include "bb_clock.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

static const char *TAG = "bb_cache";

// ---------------------------------------------------------------------------
// Registry entry
// ---------------------------------------------------------------------------

typedef struct {
    char                 key[BB_CACHE_KEY_MAX]; // key[0] == '\0' = slot free
    const void         *(*snapshot)(void); // NULL = owned mode
    void                *owned;           // heap buffer in owned mode; NULL in getter mode
    size_t               size;            // sizeof owned struct (owned mode only)
    bb_cache_serialize_fn fn;
    bb_event_topic_t     event_topic;     // registered event topic handle (NULL if no SSE)
    pthread_mutex_t      lock;
    bb_cache_flags_t     flags;           // BB_CACHE_FLAG_* bitmask
    char                *cached_json;     // memoized serialized "data" bytes (NULL = none yet)
    size_t               cached_len;      // strlen of cached_json
    bool                 dirty;           // true = cached_json stale, re-serialize on next get
    // Envelope sample-time (B1-570 PR-3): owned mode is stamped in
    // bb_cache_update() right after the memcpy; getter mode is stamped each
    // time snapshot() is invoked (serialize_locked / bb_cache_get_serialized).
    // Producers no longer emit their own ts_ms — bb_cache owns it and wraps
    // every serialize point ({"ts_ms":N,"data":{...}}).
    int64_t              ts_ms;
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
            s_entries[i].key[0] = '\0';
        }
        s_initialized = true;
    }
    pthread_mutex_unlock(&s_reg_lock);
}

static bb_cache_entry_t *find_entry(const char *key)
{
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] != '\0' && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

// Runtime lookup helper: takes s_reg_lock for the scan, releases it before
// returning. Entries are add-only at runtime (destroyed only by
// bb_cache_reset_for_test), so a returned pointer stays valid after the lock
// is released -- no use-after-free. Callers must NOT already hold s_reg_lock
// (non-recursive) or any entry's e->lock (lock ordering: s_reg_lock is always
// acquired/released before an entry's own lock, never nested inside it).
static bb_cache_entry_t *find_entry_locked(const char *key)
{
    pthread_mutex_lock(&s_reg_lock);
    bb_cache_entry_t *e = find_entry(key);
    pthread_mutex_unlock(&s_reg_lock);
    return e;
}

// Serialize entry contents into obj under the entry's lock.
// Entry must not be NULL. Caller holds no lock before calling.
// out_ts_ms, when non-NULL, receives the entry's envelope sample-time: for
// getter-mode entries this call stamps ts_ms = now (the read IS the sample);
// for owned-mode entries ts_ms was already stamped by the last bb_cache_update.
static bb_err_t serialize_locked(bb_cache_entry_t *e, bb_json_t obj, int64_t *out_ts_ms)
{
    pthread_mutex_lock(&e->lock);

    const void *snap;
    if (e->snapshot) {
        snap = e->snapshot();
        e->ts_ms = (int64_t)bb_clock_now_ms64();
    } else {
        snap = e->owned;
    }

    if (!snap) {
        pthread_mutex_unlock(&e->lock);
        bb_log_w(TAG, "key '%s': no snapshot available", e->key);
        return BB_ERR_INVALID_STATE;
    }

    e->fn(obj, snap);
    if (out_ts_ms) *out_ts_ms = e->ts_ms;
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_cache_register(const bb_cache_config_t *cfg)
{
    if (!cfg || !cfg->key || !cfg->serialize) return BB_ERR_INVALID_ARG;
    if (strlen(cfg->key) >= BB_CACHE_KEY_MAX) {
        bb_log_e(TAG, "key '%s' too long (max %d chars)", cfg->key, BB_CACHE_KEY_MAX - 1);
        return BB_ERR_INVALID_ARG;
    }

    ensure_init();

    pthread_mutex_lock(&s_reg_lock);

    // Idempotent: already registered?
    if (find_entry(cfg->key)) {
        pthread_mutex_unlock(&s_reg_lock);
        return BB_OK;
    }

    // Find a free slot
    bb_cache_entry_t *slot = NULL;
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] == '\0') {
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
    if (!cfg->snapshot) {
        if (cfg->snap_size == 0) {
            pthread_mutex_unlock(&s_reg_lock);
            return BB_ERR_INVALID_ARG;
        }
        owned = bb_calloc_prefer_spiram(1, cfg->snap_size);
        if (!owned) {
            pthread_mutex_unlock(&s_reg_lock);
            return BB_ERR_NO_SPACE;
        }
    }

    // Register event topic only when SSE flag is set.
    // Sink-only entries (no SSE delivery) skip this — bb_cache_post returns
    // BB_ERR_INVALID_STATE when event_topic is NULL, guarding against misuse.
    bb_event_topic_t ev_topic = NULL;
    if (cfg->flags & BB_CACHE_FLAG_SSE) {
        bb_event_topic_register(cfg->key, &ev_topic);
    }

    // Init per-entry mutex (recursive so serialize_locked can be called
    // from within a locked section without deadlock in future use)
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&slot->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    strncpy(slot->key, cfg->key, sizeof(slot->key) - 1);
    slot->key[sizeof(slot->key) - 1] = '\0';
    slot->snapshot    = cfg->snapshot;
    slot->owned       = owned;
    slot->size        = cfg->snap_size;
    slot->fn          = cfg->serialize;
    slot->event_topic = ev_topic;
    slot->flags       = cfg->flags;
    slot->cached_json = NULL;
    slot->cached_len  = 0;
    slot->dirty       = true;   // no bytes cached yet
    slot->ts_ms       = 0;      // stamped on first update()/snapshot() read

    pthread_mutex_unlock(&s_reg_lock);
    return BB_OK;
}

bb_err_t bb_cache_update(const char *key, const void *snap)
{
    if (!key || !snap) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry_locked(key);
    if (!e) return BB_ERR_NOT_FOUND;

    // Getter-mode: caller owns the struct; update is a no-op
    if (e->snapshot) return BB_OK;

    pthread_mutex_lock(&e->lock);
    memcpy(e->owned, snap, e->size);
    e->ts_ms = (int64_t)bb_clock_now_ms64();  // envelope sample-time (owned mode)
    e->dirty = true;   // invalidate memoized bytes; do NOT serialize here
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

bb_err_t bb_cache_post(const char *key)
{
    if (!key) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry_locked(key);
    if (!e) return BB_ERR_NOT_FOUND;
    if (!e->event_topic) return BB_ERR_INVALID_STATE;

    // Envelope (B1-570 PR-3): {"ts_ms":N,"data":{...}}. Serialize the
    // producer's fields into a nested "data" object rather than round-tripping
    // through a string, then attach ts_ms + data to the envelope root.
    bb_json_t data = bb_json_obj_new();
    if (!data) return BB_ERR_NO_SPACE;

    int64_t ts_ms = 0;
    bb_err_t err = serialize_locked(e, data, &ts_ms);
    if (err != BB_OK) {
        bb_json_free(data);
        return err;
    }

    bb_json_t root = bb_json_obj_new();
    if (!root) {
        bb_json_free(data);
        return BB_ERR_NO_SPACE;
    }
    bb_json_obj_set_int(root, "ts_ms", ts_ms);
    bb_json_obj_set_obj(root, "data", data);  // ownership of data transfers to root

    char *payload = bb_json_serialize(root);
    bb_json_free(root);
    if (!payload) return BB_ERR_NO_SPACE;

    size_t len = strlen(payload);
    err = bb_event_post(e->event_topic, 0, payload, len + 1);
    bb_json_free_str(payload);
    return err;
}

bb_err_t bb_cache_serialize_into(const char *key, bb_json_t obj)
{
    if (!key || !obj) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry_locked(key);
    if (!e) return BB_ERR_NOT_FOUND;

    // No envelope here by design — this call embeds the key's fields directly
    // as a section of a larger composed document (see header comment).
    return serialize_locked(e, obj, NULL);
}

bb_err_t bb_cache_post_serialized(const char *key, const char *json, size_t json_len)
{
    if (!key || !json) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry_locked(key);
    if (!e) return BB_ERR_NOT_FOUND;
    if (!e->event_topic) return BB_ERR_INVALID_STATE;

    return bb_event_post(e->event_topic, 0, json, json_len + 1);
}

bb_err_t bb_cache_get_serialized(const char *key, char *buf, size_t cap, size_t *out_len)
{
    if (!key || !buf || cap == 0) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry_locked(key);
    if (!e) return BB_ERR_NOT_FOUND;

    pthread_mutex_lock(&e->lock);

    // Getter-mode entries have no dirty signal (data can change without an
    // update), so always re-serialize. Owned-mode entries memoize via dirty.
    bool need = e->dirty || e->cached_json == NULL || e->snapshot != NULL;
    if (need) {
        const void *snap;
        if (e->snapshot) {
            snap = e->snapshot();
            e->ts_ms = (int64_t)bb_clock_now_ms64();  // getter mode: read IS the sample
        } else {
            snap = e->owned;
        }
        if (!snap) {
            pthread_mutex_unlock(&e->lock);
            bb_log_w(TAG, "key '%s': no snapshot available", e->key);
            return BB_ERR_INVALID_STATE;
        }

        bb_json_t obj = bb_json_obj_new();
        if (!obj) {
            pthread_mutex_unlock(&e->lock);
            return BB_ERR_NO_SPACE;
        }

        // The serializer runs exactly once per generation here. cached_json
        // holds only the inner "data" bytes -- the envelope ({"ts_ms":N,
        // "data":...}) is applied below, around the memoized string, on every
        // read (owned mode: ts_ms is frozen between updates, so the envelope
        // bytes stay byte-identical across reads within an interval).
        e->fn(obj, snap);
        char *s = bb_json_serialize(obj);
        bb_json_free(obj);
        if (!s) {
            pthread_mutex_unlock(&e->lock);
            return BB_ERR_NO_SPACE;
        }

        if (e->cached_json) bb_json_free_str(e->cached_json);
        e->cached_json = s;
        e->cached_len  = strlen(s);
        e->dirty       = false;
    }

    // Wrap the memoized "data" bytes in the envelope and copy out under the
    // lock. Compute the required length first (snprintf(NULL,0,...)) so an
    // undersized buffer is refused WITHOUT a partial write, matching the
    // pre-envelope contract ("buf untouched" on BB_ERR_NO_SPACE).
    int need_len = snprintf(NULL, 0, "{\"ts_ms\":%" PRId64 ",\"data\":%s}",
                             e->ts_ms, e->cached_json);
    if (need_len < 0) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NO_SPACE;
    }
    if ((size_t)need_len + 1 > cap) {
        pthread_mutex_unlock(&e->lock);
        bb_log_w(TAG, "key '%s': buffer too small (need %d, cap %zu)",
                 key, need_len + 1, cap);
        return BB_ERR_NO_SPACE;
    }
    snprintf(buf, cap, "{\"ts_ms\":%" PRId64 ",\"data\":%s}", e->ts_ms, e->cached_json);
    if (out_len) *out_len = (size_t)need_len;

    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Keyed enumeration + compact struct-read accessor
// ---------------------------------------------------------------------------

size_t bb_cache_count(void)
{
    ensure_init();

    size_t count = 0;
    pthread_mutex_lock(&s_reg_lock);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] != '\0') count++;
    }
    pthread_mutex_unlock(&s_reg_lock);
    return count;
}

bb_err_t bb_cache_key_at(size_t index, const char **out_key)
{
    if (!out_key) return BB_ERR_INVALID_ARG;
    if (index >= (size_t)BB_CACHE_MAX_TOPICS) return BB_ERR_NOT_FOUND;

    ensure_init();

    pthread_mutex_lock(&s_reg_lock);
    *out_key = (s_entries[index].key[0] != '\0') ? s_entries[index].key : NULL;
    pthread_mutex_unlock(&s_reg_lock);
    return BB_OK;
}

bb_err_t bb_cache_foreach(void (*cb)(const char *key, void *ctx), void *ctx)
{
    if (!cb) return BB_ERR_INVALID_ARG;

    ensure_init();

    // Snapshot key pointers under the registry lock, then release before
    // invoking cb — avoids reentrancy deadlock if cb calls back into
    // bb_cache. Pointers stay valid: bb_cache has no unregister, keys only
    // get added.
    const char *keys[BB_CACHE_MAX_TOPICS];
    int n = 0;

    pthread_mutex_lock(&s_reg_lock);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] != '\0') keys[n++] = s_entries[i].key;
    }
    pthread_mutex_unlock(&s_reg_lock);

    for (int i = 0; i < n; i++) {
        cb(keys[i], ctx);
    }
    return BB_OK;
}

bb_err_t bb_cache_get_raw(const char *key, void *buf, size_t cap)
{
    if (!key || !buf || cap == 0) return BB_ERR_INVALID_ARG;

    ensure_init();

    bb_cache_entry_t *e = find_entry_locked(key);
    if (!e) return BB_ERR_NOT_FOUND;

    pthread_mutex_lock(&e->lock);
    if (!e->owned) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_INVALID_STATE;
    }
    if (cap < e->size) {
        pthread_mutex_unlock(&e->lock);
        bb_log_w(TAG, "get_raw '%s': buf too small (%zu < %zu)", key, cap, e->size);
        return BB_ERR_NO_SPACE;
    }
    memcpy(buf, e->owned, e->size);
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Test reset (guarded by BB_CACHE_TESTING)
// ---------------------------------------------------------------------------

#ifdef BB_CACHE_TESTING
void bb_cache_reset_for_test(void)
{
    pthread_mutex_lock(&s_reg_lock);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] != '\0') {
            pthread_mutex_destroy(&s_entries[i].lock);
            bb_mem_free(s_entries[i].owned);
            if (s_entries[i].cached_json) bb_json_free_str(s_entries[i].cached_json);
            s_entries[i].key[0]      = '\0';
            s_entries[i].owned       = NULL;
            s_entries[i].snapshot    = NULL;
            s_entries[i].fn          = NULL;
            s_entries[i].event_topic = NULL;
            s_entries[i].flags       = BB_CACHE_FLAG_NONE;
            s_entries[i].cached_json = NULL;
            s_entries[i].cached_len  = 0;
            s_entries[i].dirty       = true;
            s_entries[i].ts_ms       = 0;
        }
    }
    s_initialized = false;
    pthread_mutex_unlock(&s_reg_lock);
}
#endif
