// Must come before any system header on Linux — glibc gates pthread extras
// (used implicitly via bb_cache's header contract) on _GNU_SOURCE. Mirrors
// platform/espidf/bb_cache/bb_cache_espidf.c.
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "bb_sub.h"
#include "bb_cache.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_mem.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_sub";

#define BB_SUB_TOPIC_MAX 96

// ---------------------------------------------------------------------------
// Owned cache snapshot — the raw JSON payload, exactly as received. Sized to
// BB_SUB_MAX_PAYLOAD_BYTES (Kconfig-bridged); bb_cache copies this whole
// struct byte-for-byte via bb_cache_update().
// ---------------------------------------------------------------------------

typedef struct {
    char   json[BB_SUB_MAX_PAYLOAD_BYTES];
    size_t len;
} bb_sub_snap_t;

// ---------------------------------------------------------------------------
// Seen-topics registry — bb_sub's own bookkeeping of ingress topics it has
// routed at least once. bb_cache copies the key it is given (see bb_cache.h),
// so this registry is no longer needed for pointer-lifetime reasons; it
// remains for the cache_registered dedup flag below and bb_sub's own
// BB_SUB_MAX_TOPICS-capped drop accounting (bb_sub_dropped_count()).
// ---------------------------------------------------------------------------

typedef struct {
    char topic[BB_SUB_TOPIC_MAX];
    bool used;
    // Set once bb_cache_register() has succeeded for this topic, so
    // route() can skip the redundant idempotent re-register (global lock +
    // O(N) scan inside bb_cache) on every subsequent message. Cleared by
    // bb_sub_reset_for_test() along with the rest of the entry — see that
    // function's ordering-requirement comment.
    bool cache_registered;
} bb_sub_entry_t;

static bb_sub_entry_t   s_seen[BB_SUB_MAX_TOPICS];
static uint32_t         s_dropped     = 0;
static bb_event_topic_t s_event_topic = NULL;
static pthread_mutex_t  s_lock        = PTHREAD_MUTEX_INITIALIZER;

// find_or_reserve — returns a persistent pointer (into s_seen[]) for topic,
// reserving a new slot if this is the first sighting. Returns NULL if the
// registry is full and topic is not already tracked. Caller holds s_lock.
// Caller (bb_sub_route) has already rejected topics >= sizeof(entry.topic),
// so the snprintf below never truncates.
static bb_sub_entry_t *find_or_reserve(const char *topic)
{
    for (int i = 0; i < BB_SUB_MAX_TOPICS; i++) {
        if (s_seen[i].used && strcmp(s_seen[i].topic, topic) == 0) {
            return &s_seen[i];
        }
    }
    for (int i = 0; i < BB_SUB_MAX_TOPICS; i++) {
        if (!s_seen[i].used) {
            snprintf(s_seen[i].topic, sizeof(s_seen[i].topic), "%s", topic);
            s_seen[i].used = true;
            return &s_seen[i];
        }
    }
    return NULL;
}

static void ensure_event_topic(void)
{
    if (s_event_topic) return;
    pthread_mutex_lock(&s_lock);
    if (!s_event_topic) {  // LCOV_EXCL_BR_LINE — racy arm of the DCL, unreachable single-threaded
        bb_err_t rc = bb_event_topic_register(BB_SUB_EVENT_TOPIC, &s_event_topic);
        if (rc != BB_OK) {
            bb_log_w(TAG, "failed to register aggregate event topic '%s': %d",
                     BB_SUB_EVENT_TOPIC, rc);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

// ---------------------------------------------------------------------------
// Passthrough serializer — payload is already serialized JSON. Parse it and
// copy each top-level object key into obj as-is (preserving type/structure)
// rather than hand-rolling a per-topic struct/serializer pair. Non-object
// payloads (or a parse failure) leave obj untouched (serializes as {}).
// ---------------------------------------------------------------------------

static void sub_merge_child(const char *key, bb_json_t child, void *ctx)
{
    bb_json_t obj = (bb_json_t)ctx;
    char *s = bb_json_item_serialize(child);
    if (!s) return;  // LCOV_EXCL_LINE — json-serialize OOM; bb_json's host hook doesn't guard item_serialize yet (follow-up)
    bb_json_obj_set_raw(obj, key, s);
    bb_json_free_str(s);
}

static void sub_passthrough_serialize(bb_json_t obj, const void *snap_raw)
{
    const bb_sub_snap_t *snap = (const bb_sub_snap_t *)snap_raw;
    if (snap->len == 0) return;

    bb_json_t parsed = bb_json_parse(snap->json, snap->len);
    if (!parsed) return;

    if (bb_json_get_kind(parsed) == BB_JSON_KIND_OBJECT) {
        bb_json_walk_children(parsed, sub_merge_child, obj);
    }
    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_sub_route(const char *topic, const char *payload, size_t len)
{
    if (!topic || !topic[0] || !payload) return BB_ERR_INVALID_ARG;

    if (strlen(topic) >= sizeof(((bb_sub_entry_t *)0)->topic)) {
        bb_log_w(TAG, "topic '%s' is %d chars or longer (max %d); rejected, not truncated",
                 topic, BB_SUB_TOPIC_MAX, BB_SUB_TOPIC_MAX - 1);
        return BB_ERR_INVALID_ARG;
    }

    if (len >= sizeof(((bb_sub_snap_t *)0)->json)) {
        bb_log_w(TAG, "topic '%s': payload %zu bytes exceeds cap %d bytes; dropped",
                 topic, len, BB_SUB_MAX_PAYLOAD_BYTES);
        pthread_mutex_lock(&s_lock);
        s_dropped++;
        pthread_mutex_unlock(&s_lock);
        return BB_ERR_NO_SPACE;
    }

    ensure_event_topic();

    pthread_mutex_lock(&s_lock);
    bb_sub_entry_t *entry = find_or_reserve(topic);
    if (!entry) s_dropped++;
    pthread_mutex_unlock(&s_lock);

    if (!entry) {
        bb_log_w(TAG, "seen-topics registry full (max %d); dropped '%s'",
                 BB_SUB_MAX_TOPICS, topic);
        return BB_ERR_NO_SPACE;
    }

    pthread_mutex_lock(&s_lock);
    bool need_register = !entry->cache_registered;
    pthread_mutex_unlock(&s_lock);

    if (need_register) {
        bb_cache_config_t cache_cfg = {
            .key       = entry->topic,
            .snapshot  = NULL,
            .snap_size = sizeof(bb_sub_snap_t),
            .serialize = sub_passthrough_serialize,
            .flags     = BB_CACHE_FLAG_SSE,
        };
        bb_err_t rc = bb_cache_register(&cache_cfg);
        if (rc != BB_OK) {
            // bb_cache's own registry (a separate cap from bb_sub's local
            // seen-topics registry above) is full.
            pthread_mutex_lock(&s_lock);
            s_dropped++;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "bb_cache registry full; dropped '%s': %d", topic, rc);
            return rc;
        }
        pthread_mutex_lock(&s_lock);
        entry->cache_registered = true;
        pthread_mutex_unlock(&s_lock);
    }

    // Heap-allocated (not stack): sized up to BB_SUB_MAX_PAYLOAD_BYTES
    // (Kconfig-tunable, up to 4096) and bb_cache_update() memcpy's it
    // straight into its own owned buffer, so the stack copy is pure
    // passthrough with no caller-stack contract. Matters most for callers
    // on tight-stack tasks (e.g. the MQTT event callback).
    bb_sub_snap_t *snap = (bb_sub_snap_t *)bb_malloc_prefer_spiram(sizeof(bb_sub_snap_t));
    if (!snap) {
        pthread_mutex_lock(&s_lock);
        s_dropped++;
        pthread_mutex_unlock(&s_lock);
        bb_log_w(TAG, "topic '%s': snapshot alloc (%zu bytes) failed; dropped",
                 topic, sizeof(bb_sub_snap_t));
        return BB_ERR_NO_MEM;
    }
    memset(snap, 0, sizeof(*snap));
    memcpy(snap->json, payload, len);
    snap->json[len] = '\0';
    snap->len = len;

    // bb_cache_update makes the routed data visible to later pull reads
    // (bb_cache_get_serialized / bb_cache_serialize_into); post_serialized
    // additionally delivers the SAME raw bytes immediately to any direct
    // bb_event subscriber of the per-topic event (no per-topic SSE ring —
    // that's a separate, opt-in attach step by the consumer).
    bb_cache_update(entry->topic, snap);
    bb_cache_post_serialized(entry->topic, payload, len);
    bb_mem_free(snap);

    if (s_event_topic) {
        bb_event_post(s_event_topic, 0, entry->topic, strlen(entry->topic) + 1);
    }

    return BB_OK;
}

uint32_t bb_sub_dropped_count(void)
{
    pthread_mutex_lock(&s_lock);
    uint32_t n = s_dropped;
    pthread_mutex_unlock(&s_lock);
    return n;
}

bb_err_t bb_sub_subscribe(bb_event_handler_fn cb, void *user, bb_event_sub_t *out_sub)
{
    if (!cb || !out_sub) return BB_ERR_INVALID_ARG;
    ensure_event_topic();
    if (!s_event_topic) return BB_ERR_INVALID_STATE;
    return bb_event_subscribe(s_event_topic, cb, user, out_sub);
}

#ifdef BB_SUB_TESTING
// ORDERING REQUIREMENT: if the test also resets bb_cache, call
// bb_cache_reset_for_test() BEFORE this function (see bb_sub.h). Clearing
// s_seen here (including each entry's cache_registered flag) makes the next
// bb_sub_route() for that topic re-register it in bb_cache; if bb_cache was
// reset FIRST while a since-cleared entry still had cache_registered=true,
// there is a window where bb_sub believes a topic is registered in bb_cache
// but it isn't — bb_cache_update()/bb_cache_post_serialized() would then
// fail with BB_ERR_NOT_FOUND. Resetting bb_cache first, then bb_sub, avoids
// that window entirely.
void bb_sub_reset_for_test(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_seen, 0, sizeof(s_seen));
    s_dropped     = 0;
    // NOTE: s_event_topic is intentionally left as-is. bb_event's own topic
    // registry is idempotent (bb_event_topic_register returns the same
    // handle for a duplicate name) and is reset independently via
    // bb_event_reset_for_test() by the test harness; clearing it here would
    // just cause the next ensure_event_topic() call to re-resolve the same
    // handle (or a stale one, if bb_event was reset but this wasn't) — so
    // tests that reset bb_event MUST also call this function afterwards.
    s_event_topic = NULL;
    pthread_mutex_unlock(&s_lock);
}
#endif
