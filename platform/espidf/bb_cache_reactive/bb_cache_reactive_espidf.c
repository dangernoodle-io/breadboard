// Must come before any system header on Linux — glibc gates pthread APIs on
// _GNU_SOURCE (or _XOPEN_SOURCE >= 500). macOS exposes them unconditionally.
// Same idiom as platform/espidf/bb_cache/bb_cache_espidf.c.
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "bb_cache_reactive.h"

#if BB_CACHE_REACTIVE_ENABLE

#include "bb_cache.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_mem.h"

#include <string.h>
#include <stdlib.h>
#include <pthread.h>

static const char *TAG = "bb_cache_reactive";

// ---------------------------------------------------------------------------
// Observer pool
// ---------------------------------------------------------------------------

typedef struct {
    bool                     active;
    bool                     observe_all;              // key == NULL at register time
    char                     key[BB_CACHE_KEY_MAX];     // valid iff !observe_all
    bb_cache_on_register_fn  on_register;               // reserved, stored only
    bb_cache_on_change_fn    on_change;
    bb_cache_on_remove_fn    on_remove;                 // reserved, stored only
    void                    *ctx;
} reactive_observer_t;

static reactive_observer_t s_observers[BB_CACHE_REACTIVE_MAX_OBSERVERS];
// Guards s_observers[] only. Lock order (B1-589, load-bearing): any
// s_reg_lock/e->lock held by bb_cache is always acquired and released BEFORE
// this function ever takes s_obs_lock -- this file never calls into
// bb_cache's public API while holding s_obs_lock, so s_obs_lock is never
// nested inside a bb_cache-owned lock.
static pthread_mutex_t s_obs_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef BB_CACHE_REACTIVE_TESTING
// Test-injectable envelope splitter. bb_cache always emits a well-formed
// {"ts_ms":N,"data":{...}} envelope via bb_json (which can never itself
// produce unbalanced braces), so the "envelope split failed" branch below is
// otherwise unreachable through legitimate bb_cache use -- mirror the
// malloc-fail injection idiom (test_alloc_inject.h) to exercise it.
static bool (*s_envelope_split_fn)(const char *, int, const char **, size_t *,
                                    const char **, size_t *) = bb_json_envelope_split;
#define ENVELOPE_SPLIT(...) s_envelope_split_fn(__VA_ARGS__)
#else
#define ENVELOPE_SPLIT(...) bb_json_envelope_split(__VA_ARGS__)
#endif

typedef struct {
    bb_cache_on_change_fn fn;
    void                  *ctx;
} match_t;

// Snapshot every observer matching `key` (exact match or observe_all) under
// s_obs_lock, release the lock, THEN fetch the serialized envelope and
// invoke callbacks with no lock held -- mirrors bb_cache_foreach's
// snapshot-then-notify shape so an on_change callback may safely call back
// into bb_cache (or register a new observer) without deadlocking.
static void fire_on_change(const char *key)
{
    match_t matches[BB_CACHE_REACTIVE_MAX_OBSERVERS];
    int n = 0;

    pthread_mutex_lock(&s_obs_lock);
    for (int i = 0; i < BB_CACHE_REACTIVE_MAX_OBSERVERS; i++) {
        reactive_observer_t *o = &s_observers[i];
        if (!o->active || !o->on_change) continue;
        if (o->observe_all || strcmp(o->key, key) == 0) {
            matches[n].fn  = o->on_change;
            matches[n].ctx = o->ctx;
            n++;
        }
    }
    pthread_mutex_unlock(&s_obs_lock);

    if (n == 0) return;

    // Heap/PSRAM-allocate the fetch buffer rather than stack -- this fires at
    // change cadence (bounded churn), and BB_CACHE_REACTIVE_PAYLOAD_MAX is
    // caller-tunable up to 4096 bytes via Kconfig, which would otherwise
    // inflate every reactive_update caller's stack (mirrors the
    // bb_malloc_prefer_spiram + bb_mem_free idiom already used by
    // bb_sink_ws for its envelope buffers).
    char *buf = (char *)bb_malloc_prefer_spiram(BB_CACHE_REACTIVE_PAYLOAD_MAX);
    if (!buf) {
        bb_log_w(TAG, "fire_on_change: malloc failed for '%s'", key);
        return;
    }

    size_t len = 0;
    if (bb_cache_get_serialized(key, buf, BB_CACHE_REACTIVE_PAYLOAD_MAX, &len) != BB_OK) {
        bb_log_w(TAG, "fire_on_change: get_serialized('%s') failed", key);
        bb_mem_free(buf);
        return;
    }

    const char *ts_start = NULL, *data_start = NULL;
    size_t ts_len = 0, data_len = 0;
    if (!ENVELOPE_SPLIT(buf, (int)len, &ts_start, &ts_len, &data_start, &data_len)) {
        bb_log_w(TAG, "fire_on_change: envelope split failed for '%s'", key);
        bb_mem_free(buf);
        return;
    }

    // ts_start/ts_len come from splitting bb_cache's OWN envelope, which
    // always stamps ts_ms as a plain int64 decimal literal (max 20 chars
    // incl. sign) -- comfortably under sizeof(ts_buf), no runtime clamp
    // needed.
    char ts_buf[24];
    memcpy(ts_buf, ts_start, ts_len);
    ts_buf[ts_len] = '\0';
    int64_t ts_ms = (int64_t)strtoll(ts_buf, NULL, 10);

    for (int i = 0; i < n; i++) {
        matches[i].fn(key, data_start, data_len, ts_ms, matches[i].ctx);
    }
    bb_mem_free(buf);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_cache_reactive_observe(const bb_cache_reactive_observer_t *cfg)
{
    if (!cfg) return BB_ERR_INVALID_ARG;
    if (cfg->key && strlen(cfg->key) >= BB_CACHE_KEY_MAX) {
        bb_log_e(TAG, "observe: key '%s' too long (max %d chars)", cfg->key, BB_CACHE_KEY_MAX - 1);
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&s_obs_lock);

    reactive_observer_t *slot = NULL;
    for (int i = 0; i < BB_CACHE_REACTIVE_MAX_OBSERVERS; i++) {
        if (!s_observers[i].active) {
            slot = &s_observers[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&s_obs_lock);
        bb_log_e(TAG, "observe: pool full (max %d)", BB_CACHE_REACTIVE_MAX_OBSERVERS);
        return BB_ERR_NO_SPACE;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    if (cfg->key) {
        slot->observe_all = false;
        strncpy(slot->key, cfg->key, sizeof(slot->key) - 1);
        slot->key[sizeof(slot->key) - 1] = '\0';
    } else {
        slot->observe_all = true;
    }
    slot->on_register = cfg->on_register;
    slot->on_change   = cfg->on_change;
    slot->on_remove   = cfg->on_remove;
    slot->ctx         = cfg->ctx;

    pthread_mutex_unlock(&s_obs_lock);
    return BB_OK;
}

bb_err_t bb_cache_reactive_update(const bb_cache_update_t *req)
{
    if (!req) return BB_ERR_INVALID_ARG;

    bool changed = false;
    bb_cache_update_t local = *req;
    local.out_changed = &changed;

    bb_err_t err = bb_cache_update(&local);
    if (req->out_changed) *req->out_changed = changed;

    // req->key is guaranteed non-NULL here: bb_cache_update() itself rejects
    // a NULL key with BB_ERR_INVALID_ARG before this point, so err == BB_OK
    // already implies a valid key.
    if (err == BB_OK && changed) {
        fire_on_change(req->key);
    }
    return err;
}

#ifdef BB_CACHE_REACTIVE_TESTING
void bb_cache_reactive_reset_for_test(void)
{
    pthread_mutex_lock(&s_obs_lock);
    memset(s_observers, 0, sizeof(s_observers));
    pthread_mutex_unlock(&s_obs_lock);
    s_envelope_split_fn = bb_json_envelope_split;
}

// Inject a fake envelope splitter to exercise fire_on_change's
// "envelope split failed" branch (otherwise unreachable -- see the
// s_envelope_split_fn comment above). Pass NULL to restore the real
// bb_json_envelope_split.
void bb_cache_reactive_set_envelope_split_for_test(
    bool (*fn)(const char *, int, const char **, size_t *, const char **, size_t *))
{
    s_envelope_split_fn = fn ? fn : bb_json_envelope_split;
}
#endif

#endif // BB_CACHE_REACTIVE_ENABLE
