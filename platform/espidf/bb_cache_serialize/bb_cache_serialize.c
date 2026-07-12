// Must come before any system header on Linux -- glibc gates
// PTHREAD_MUTEX_RECURSIVE on _GNU_SOURCE (or _XOPEN_SOURCE >= 500). macOS
// exposes it unconditionally. Not needed here (the mutex is a plain default
// type, no recursion), but matches the codebase convention for the first
// line of every pthread-using platform TU.
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

// Deferred work: bb_cache_serialize_get()'s borrowed-pointer output is a
// single-threaded-consumer contract (see the public header's doc comment for
// the full hazard). A hardened copy-out variant (caller-supplied buffer,
// copies bytes under the lock instead of returning a borrowed pointer) is
// the planned fix for multi-threaded consumers -- not yet wired up.

#include "bb_cache_serialize.h"

#include "bb_cache.h"
#include "bb_log.h"
#include "bb_serialize_json.h"

#include <pthread.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Capacity constants (Kconfig bridge -- pattern from bb_cache.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_CACHE_SERIALIZE_MAX_ENTRIES
#define BB_CACHE_SERIALIZE_MAX_ENTRIES CONFIG_BB_CACHE_SERIALIZE_MAX_ENTRIES
#endif
#ifdef CONFIG_BB_CACHE_SERIALIZE_BUF_BYTES
#define BB_CACHE_SERIALIZE_BUF_BYTES CONFIG_BB_CACHE_SERIALIZE_BUF_BYTES
#endif
#endif
#ifndef BB_CACHE_SERIALIZE_MAX_ENTRIES
#define BB_CACHE_SERIALIZE_MAX_ENTRIES 8
#endif
#ifndef BB_CACHE_SERIALIZE_BUF_BYTES
#define BB_CACHE_SERIALIZE_BUF_BYTES 512
#endif

static const char *TAG = "bb_cache_serialize";

// ---------------------------------------------------------------------------
// Slot table -- one memo per (format, key) pair, fixed-size, no heap.
// ---------------------------------------------------------------------------

typedef struct {
    bool        valid;
    bb_format_t fmt;
    char        key[BB_CACHE_KEY_MAX];   // by-value copy, UAF-safe
    uint32_t    version;
    size_t      len;
    uint8_t     buf[BB_CACHE_SERIALIZE_BUF_BYTES];
} bb_cache_serialize_slot_t;

static bb_cache_serialize_slot_t s_slots[BB_CACHE_SERIALIZE_MAX_ENTRIES];
static pthread_mutex_t           s_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef BB_CACHE_SERIALIZE_TESTING
static size_t s_render_count;
#endif

// Caller holds s_lock. Linear scan for a valid slot matching both fmt and
// key -- the table is sized for a handful of registered wire endpoints, so a
// linear scan is cheap and needs no auxiliary index.
static bb_cache_serialize_slot_t *find_slot_locked(bb_format_t fmt, const char *key)
{
    for (size_t i = 0; i < BB_CACHE_SERIALIZE_MAX_ENTRIES; i++) {
        if (s_slots[i].valid && s_slots[i].fmt == fmt && strcmp(s_slots[i].key, key) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

// Caller holds s_lock. First slot with valid == false -- a slot is claimed
// (key/fmt stamped) but left invalid until a render actually succeeds, so a
// failed render never leaves a stale-valid entry behind.
static bb_cache_serialize_slot_t *find_free_slot_locked(void)
{
    for (size_t i = 0; i < BB_CACHE_SERIALIZE_MAX_ENTRIES; i++) {
        if (!s_slots[i].valid) return &s_slots[i];
    }
    return NULL;
}

bb_err_t bb_cache_serialize_get(bb_format_t fmt, const char *key,
                                 const bb_serialize_desc_t *desc,
                                 const uint8_t **out, size_t *out_len)
{
    if (!key || !desc || !out || !out_len) return BB_ERR_INVALID_ARG;

    pthread_mutex_lock(&s_lock);

    bb_cache_serialize_slot_t *slot = find_slot_locked(fmt, key);

    // Format support is a STATIC property, independent of whether `key` is
    // registered -- check it BEFORE calling into bb_cache (state_version
    // below), so an unsupported fmt always returns UNSUPPORTED, even for a
    // key that was never registered (a key-existence check reached first
    // would otherwise mask this as NOT_FOUND). The local slot-table lookup
    // just above is cheap (no bb_cache call, s_lock only) and stays ahead of
    // this check so an existing memo for `key` under a DIFFERENT fmt is
    // still found before falling through to the fmt gate. JSON is the only
    // wired backend today; a future format backend adds its own branch
    // here, not a registry.
    if (fmt != BB_FORMAT_JSON) {
        pthread_mutex_unlock(&s_lock);
        return BB_ERR_UNSUPPORTED;
    }

    // Lock order: s_lock is held across this call into bb_cache, which
    // internally takes bb_cache's own per-entry lock (bb_cache_state_version
    // / bb_cache_snapshot below) -- so the order is s_lock -> bb_cache
    // e->lock. The reverse must NEVER exist (bb_cache must never call back
    // into bb_cache_serialize while holding e->lock), or this deadlocks.
    uint32_t cur = 0;
    bb_err_t rc = bb_cache_state_version(key, &cur);
    if (rc != BB_OK) {
        pthread_mutex_unlock(&s_lock);
        return rc;
    }

    // slot->valid is not re-checked here: find_slot_locked() only ever
    // returns a slot with valid == true (an invalid slot is invisible to it
    // -- see that function's doc comment), so a non-NULL slot here is
    // always valid by construction.
    if (slot && slot->version == cur) {
        *out     = slot->buf;
        *out_len = slot->len;
        pthread_mutex_unlock(&s_lock);
        return BB_OK;
    }

    // MISS / recache. fmt support was already validated above, before this
    // point -- reaching here means fmt == BB_FORMAT_JSON.
    if (!slot) {
        slot = find_free_slot_locked();
        if (!slot) {
            pthread_mutex_unlock(&s_lock);
            bb_log_e(TAG, "slot table full (max %d)", BB_CACHE_SERIALIZE_MAX_ENTRIES);
            return BB_ERR_NO_SPACE;
        }
        slot->fmt = fmt;
        strncpy(slot->key, key, sizeof(slot->key) - 1);
        slot->key[sizeof(slot->key) - 1] = '\0';
    }

    uint8_t tmp[BB_CACHE_SERIALIZE_BUF_BYTES];
    bb_cache_snapshot_t snap = {0};
    rc = bb_cache_snapshot(key, tmp, sizeof(tmp), &snap);
    if (rc != BB_OK) {
        slot->valid = false;
        pthread_mutex_unlock(&s_lock);
        return rc;
    }

    // Render happens under s_lock, so ALL gets across ALL keys serialize
    // behind this single mutex for the render duration -- acceptable at the
    // current small MAX_ENTRIES; revisit (e.g. per-slot locks) if the table
    // grows large enough for render latency to matter.
    size_t n = 0;
    rc = bb_serialize_json_render(desc, snap.state, (char *)slot->buf, sizeof(slot->buf), &n);
#ifdef BB_CACHE_SERIALIZE_TESTING
    s_render_count++;
#endif
    if (rc != BB_OK) {
        slot->valid = false;
        pthread_mutex_unlock(&s_lock);
        bb_log_w(TAG, "key '%s': render overflow (cap %d)", key, BB_CACHE_SERIALIZE_BUF_BYTES);
        return rc;
    }

    slot->version = snap.version;
    slot->len     = n;
    slot->valid   = true;

    *out     = slot->buf;
    *out_len = slot->len;
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

#ifdef BB_CACHE_SERIALIZE_TESTING
void bb_cache_serialize_reset_for_test(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_slots, 0, sizeof(s_slots));
    s_render_count = 0;
    pthread_mutex_unlock(&s_lock);
}

size_t bb_cache_serialize_render_count(void)
{
    return s_render_count;
}
#endif
