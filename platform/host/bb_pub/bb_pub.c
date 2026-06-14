// bb_pub core — transport-agnostic telemetry publisher.
// Compiled on both host (tests) and ESP-IDF (linked alongside bb_pub_espidf.c).
//
// Timestamp note: bb_pub_tick_once injects a "ts" field using
// bb_clock_now_ms(), which returns uptime-milliseconds on all platforms
// (host: CLOCK_MONOTONIC, ESP-IDF: esp_timer_get_time()/1000). On devices
// with NTP synchronised wall-clock time, callers wanting epoch-ms should
// include the wall-clock timestamp themselves inside their sample_fn.
#include "bb_pub.h"
#include "bb_clock.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>

// CONFIG_BB_PUB_MAX_SOURCES, CONFIG_BB_PUB_MAX_SINKS,
// CONFIG_BB_PUB_TOPIC_PREFIX, and CONFIG_BB_PUB_INTERVAL_MS are provided by
// Kconfig on ESP-IDF; supply defaults for host builds.
#ifndef CONFIG_BB_PUB_MAX_SOURCES
#define CONFIG_BB_PUB_MAX_SOURCES 8
#endif
#ifndef CONFIG_BB_PUB_MAX_SINKS
#define CONFIG_BB_PUB_MAX_SINKS 4
#endif
#ifndef BB_PUB_MAX_PAYLOAD_EXTENDERS
#define BB_PUB_MAX_PAYLOAD_EXTENDERS 4
#endif
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif
#ifndef CONFIG_BB_PUB_INTERVAL_MS
#define CONFIG_BB_PUB_INTERVAL_MS 10000
#endif

// Interval bounds (must match Kconfig range).
#define BB_PUB_INTERVAL_MS_MIN   1000UL
#define BB_PUB_INTERVAL_MS_MAX   3600000UL

// NVS namespace and keys used by bb_pub for its own persistent config.
#define BB_PUB_NVS_NS           "bb_pub"
#define BB_PUB_NVS_KEY_INTERVAL "interval_ms"
#define BB_PUB_NVS_KEY_ENABLED  "enabled"

static const char *TAG = "bb_pub";

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef struct {
    char            subtopic[64];
    bb_pub_sample_fn fn;
    void            *ctx;
} bb_pub_source_t;

static bb_pub_source_t s_sources[CONFIG_BB_PUB_MAX_SOURCES];
static int             s_source_count   = 0;
static bool            s_hwm_warned     = false;

static bb_pub_sink_t   s_sinks[CONFIG_BB_PUB_MAX_SINKS];
static int             s_sink_count     = 0;

// ---------------------------------------------------------------------------
// Payload-extender registry
// ---------------------------------------------------------------------------

typedef struct {
    bb_pub_payload_fn fn;
    void             *ctx;
} bb_pub_payload_entry_t;

static bb_pub_payload_entry_t s_payload_extenders[BB_PUB_MAX_PAYLOAD_EXTENDERS];
static int                    s_payload_extender_count = 0;
static bool                   s_payload_hwm_warned     = false;

// ---------------------------------------------------------------------------
// Runtime config (NVS-persisted)
// ---------------------------------------------------------------------------

// Effective interval in ms; loaded from NVS at first get, default = compile-time.
static uint32_t s_interval_ms  = 0;   /* 0 = not yet loaded */
// Persistent enable toggle; 1 = enabled (default).
static uint8_t  s_enabled      = 1;
// Whether s_interval_ms has been loaded from NVS.
static bool     s_config_loaded = false;

// Optional hook for live timer re-arm; set by the ESP-IDF worker.
static void (*s_interval_apply_hook)(uint32_t ms) = NULL;

// Load interval + enabled from NVS into the in-RAM cache (idempotent).
static void ensure_config_loaded(void)
{
    if (s_config_loaded) return;
    bb_nv_get_u32(BB_PUB_NVS_NS, BB_PUB_NVS_KEY_INTERVAL, &s_interval_ms,
                  (uint32_t)CONFIG_BB_PUB_INTERVAL_MS);
    bb_nv_get_u8(BB_PUB_NVS_NS, BB_PUB_NVS_KEY_ENABLED, &s_enabled, 1);
    s_config_loaded = true;
}

// ---------------------------------------------------------------------------
// Pause state
// ---------------------------------------------------------------------------

// s_paused is always read/written under s_tick_lock so no separate atomic is
// needed; the lock itself provides the visibility guarantee.
static bool s_paused = false;

// Tick lock — held for the duration of bb_pub_tick_once's active body (the
// sample → serialize → sink fan-out loop).  bb_pub_pause() sets s_paused then
// acquires+releases this lock so it cannot return while a tick is in flight.
// The lock is a plain (non-recursive) mutex; the worker holds it only for the
// tick duration so there is no inversion risk with callers of bb_pub_pause().
static pthread_mutex_t s_tick_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Status state
// ---------------------------------------------------------------------------

static bool     s_last_publish_ok  = false;
static uint32_t s_last_publish_ms  = 0;
static bool     s_published_ever   = false;

// ---------------------------------------------------------------------------
// Public API — sinks
// ---------------------------------------------------------------------------

bb_err_t bb_pub_add_sink(const bb_pub_sink_t *sink)
{
    if (!sink || !sink->publish) return BB_ERR_INVALID_ARG;

    if (s_sink_count >= CONFIG_BB_PUB_MAX_SINKS) {
        return BB_ERR_NO_SPACE;
    }
    s_sinks[s_sink_count++] = *sink;
    return BB_OK;
}

void bb_pub_clear_sinks(void)
{
    s_sink_count = 0;
}

bb_err_t bb_pub_set_sink(const bb_pub_sink_t *sink)
{
    bb_pub_clear_sinks();
    if (!sink || !sink->publish) {
        return BB_OK;
    }
    return bb_pub_add_sink(sink);
}

// ---------------------------------------------------------------------------
// Public API — status
// ---------------------------------------------------------------------------

bb_err_t bb_pub_get_status(bb_pub_status_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    out->source_count     = s_source_count;
    out->sink_count       = s_sink_count;
    out->last_publish_ok  = s_last_publish_ok;
    out->last_publish_ms  = s_last_publish_ms;
    out->published_ever   = s_published_ever;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API — sources
// ---------------------------------------------------------------------------

bb_err_t bb_pub_register_source(const char *subtopic, bb_pub_sample_fn fn, void *ctx)
{
    if (!subtopic || !fn) return BB_ERR_INVALID_ARG;

    if (s_source_count >= CONFIG_BB_PUB_MAX_SOURCES) {
        return BB_ERR_NO_SPACE;
    }

    // High-watermark warning at cap-1.
    if (!s_hwm_warned && s_source_count == CONFIG_BB_PUB_MAX_SOURCES - 1) {
        bb_log_w(TAG, "source registry at high-watermark (%d/%d)",
                 s_source_count, CONFIG_BB_PUB_MAX_SOURCES);
        s_hwm_warned = true;
    }

    bb_pub_source_t *src = &s_sources[s_source_count++];
    strncpy(src->subtopic, subtopic, sizeof(src->subtopic) - 1);
    src->subtopic[sizeof(src->subtopic) - 1] = '\0';
    src->fn  = fn;
    src->ctx = ctx;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API — payload extenders
// ---------------------------------------------------------------------------

bb_err_t bb_pub_register_payload_extender(bb_pub_payload_fn fn, void *ctx)
{
    if (!fn) return BB_ERR_INVALID_ARG;

    if (s_payload_extender_count >= BB_PUB_MAX_PAYLOAD_EXTENDERS) {
        bb_log_w(TAG, "payload extender registry full (%d/%d)",
                 s_payload_extender_count, BB_PUB_MAX_PAYLOAD_EXTENDERS);
        return BB_ERR_NO_SPACE;
    }

    // High-watermark warning at cap-1.
    if (!s_payload_hwm_warned &&
        s_payload_extender_count == BB_PUB_MAX_PAYLOAD_EXTENDERS - 1) {
        bb_log_w(TAG, "payload extender registry at high-watermark (%d/%d)",
                 s_payload_extender_count, BB_PUB_MAX_PAYLOAD_EXTENDERS);
        s_payload_hwm_warned = true;
    }

    s_payload_extenders[s_payload_extender_count].fn  = fn;
    s_payload_extenders[s_payload_extender_count].ctx = ctx;
    s_payload_extender_count++;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API — interval + enabled
// ---------------------------------------------------------------------------

bb_err_t bb_pub_set_interval_ms(uint32_t ms)
{
    if (ms < BB_PUB_INTERVAL_MS_MIN || ms > BB_PUB_INTERVAL_MS_MAX) {
        return BB_ERR_INVALID_ARG;
    }
    ensure_config_loaded();
    s_interval_ms = ms;
    bb_err_t err = bb_nv_set_u32(BB_PUB_NVS_NS, BB_PUB_NVS_KEY_INTERVAL, ms);
    if (err != BB_OK) {
        bb_log_w(TAG, "set_interval_ms: NVS write failed: %d", err);
    }
    if (s_interval_apply_hook) {
        s_interval_apply_hook(ms);
    }
    return BB_OK;
}

uint32_t bb_pub_get_interval_ms(void)
{
    ensure_config_loaded();
    return s_interval_ms;
}

bb_err_t bb_pub_set_enabled(bool en)
{
    ensure_config_loaded();
    s_enabled = en ? 1u : 0u;
    bb_err_t err = bb_nv_set_u8(BB_PUB_NVS_NS, BB_PUB_NVS_KEY_ENABLED, s_enabled);
    if (err != BB_OK) {
        bb_log_w(TAG, "set_enabled: NVS write failed: %d", err);
    }
    return BB_OK;
}

bool bb_pub_is_enabled(void)
{
    ensure_config_loaded();
    return s_enabled != 0;
}

void bb_pub_set_interval_apply_hook(void (*hook)(uint32_t ms))
{
    s_interval_apply_hook = hook;
}

// ---------------------------------------------------------------------------
// Public API — pause / resume
// ---------------------------------------------------------------------------

void bb_pub_pause(void)
{
    // Set the flag first so the worker skips the next tick body immediately.
    // Then acquire+release the tick lock to block until any currently-executing
    // tick completes.  When this function returns the caller is guaranteed that:
    //   1. no bb_pub_tick_once body is running (lock was released by worker), AND
    //   2. s_paused is true so no new tick will enter its active body.
    s_paused = true;
    pthread_mutex_lock(&s_tick_lock);
    pthread_mutex_unlock(&s_tick_lock);
}

void bb_pub_resume(void)
{
    // No lock needed: setting s_paused=false is fine outside the lock because
    // the tick body only reads it under the lock at entry; once cleared the
    // next tick will proceed normally.
    s_paused = false;
}

bool bb_pub_is_paused(void)
{
    return s_paused;
}

// ---------------------------------------------------------------------------
// Public API — tick
// ---------------------------------------------------------------------------

bb_err_t bb_pub_tick_once(void)
{
    ensure_config_loaded();
    if (!bb_pub_is_enabled()) return BB_OK;

    // Fast pre-check before taking the lock (avoids lock contention when the
    // common case is not paused).  The check is repeated under the lock below.
    if (s_paused) return BB_OK;
    if (s_sink_count == 0) return BB_OK;

    // Hold the tick lock for the entire active body so that bb_pub_pause() can
    // block here waiting for an in-flight tick to finish before returning.
    pthread_mutex_lock(&s_tick_lock);

    // Re-check under the lock: bb_pub_pause() may have set s_paused between
    // the pre-check above and our lock acquisition.
    if (s_paused) {
        pthread_mutex_unlock(&s_tick_lock);
        return BB_OK;
    }

    // Take ONE timestamp for the entire cycle.
    uint32_t ts_ms = bb_clock_now_ms();

    const char *hostname = bb_nv_config_hostname();
    if (!hostname || hostname[0] == '\0') {
        hostname = "device";
    }

    char topic[192];

    // Track whether any source emitted during this tick and whether all
    // sink calls succeeded.
    bool tick_published = false;
    bool tick_all_ok    = true;

    for (int i = 0; i < s_source_count; i++) {
        bb_pub_source_t *src = &s_sources[i];

        bb_json_t obj = bb_json_obj_new();
        if (!obj) {
            bb_log_w(TAG, "tick: failed to allocate JSON obj for '%s'", src->subtopic);
            continue;
        }

        bool publish = src->fn(obj, src->ctx);
        if (!publish) {
            bb_json_free(obj);
            continue;
        }

        // Inject shared timestamp field (uptime-ms; see file-level note above).
        bb_json_obj_set_number(obj, "ts", (double)ts_ms);

        // Invoke payload extenders in registration order before serialize.
        for (int pi = 0; pi < s_payload_extender_count; pi++) {
            s_payload_extenders[pi].fn(obj, src->subtopic,
                                       s_payload_extenders[pi].ctx);
        }

        snprintf(topic, sizeof(topic), "%s/%s/%s",
                 CONFIG_BB_PUB_TOPIC_PREFIX, hostname, src->subtopic);

        // Fan-out: deliver to every registered sink. Each sink gets its OWN
        // serialized string so per-sink transport/tls fields can be stamped.
        // A failing sink does not abort delivery to the remaining sinks.
        for (int si = 0; si < s_sink_count; si++) {
            const bb_pub_sink_t *sk = &s_sinks[si];

            // Stamp per-sink metadata fields when transport is set.
            // Use delete-before-set to avoid cJSON appending duplicate keys
            // when multiple sinks share the same obj.
            if (sk->transport) {
                bb_json_obj_delete_key(obj, "transport");
                bb_json_obj_delete_key(obj, "tls");
                bb_json_obj_set_string(obj, "transport", sk->transport);
                bb_json_obj_set_bool(obj, "tls", sk->tls);
            }

            char *json = bb_json_serialize(obj);
            if (!json) {
                bb_log_w(TAG, "tick: failed to serialize '%s' for sink[%d]",
                         src->subtopic, si);
                tick_all_ok = false;
                continue;
            }

            int json_len = (int)strlen(json);
            bb_err_t err = sk->publish(sk->ctx, topic, json, json_len);
            bb_json_free_str(json);

            if (err != BB_OK) {
                bb_log_w(TAG, "sink[%d] publish failed for '%s': %d",
                         si, src->subtopic, err);
                tick_all_ok = false;
            }
        }

        bb_json_free(obj);
        tick_published = true;
    }

    // Update status only when at least one source was published this tick.
    if (tick_published) {
        s_last_publish_ms  = ts_ms;
        s_published_ever   = true;
        s_last_publish_ok  = tick_all_ok;
    }

    pthread_mutex_unlock(&s_tick_lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Exclusive-sink arbiter
// ---------------------------------------------------------------------------

static const char *s_exclusive_holder = NULL;   /* NULL = slot free */

bb_err_t bb_pub_exclusive_acquire(const char *sink_id)
{
    if (!sink_id) return BB_ERR_INVALID_ARG;
    if (s_exclusive_holder == NULL) {
        s_exclusive_holder = sink_id;
        return BB_OK;
    }
    /* Already held by this id (idempotent) */
    if (s_exclusive_holder == sink_id ||
        strcmp(s_exclusive_holder, sink_id) == 0) {
        return BB_OK;
    }
    /* Held by a different id — conflict */
    bb_log_w(TAG, "exclusive sink conflict: '%s' holds slot, '%s' rejected",
             s_exclusive_holder, sink_id);
    return BB_ERR_CONFLICT;
}

void bb_pub_exclusive_release(const char *sink_id)
{
    if (!sink_id) return;
    if (s_exclusive_holder == NULL) return;
    if (s_exclusive_holder == sink_id ||
        strcmp(s_exclusive_holder, sink_id) == 0) {
        s_exclusive_holder = NULL;
    }
}

// ---------------------------------------------------------------------------
// Testing hooks
// ---------------------------------------------------------------------------

#ifdef BB_PUB_TESTING
void bb_pub_exclusive_reset(void)
{
    s_exclusive_holder = NULL;
}

void bb_pub_test_reset(void)
{
    s_source_count             = 0;
    s_hwm_warned               = false;
    s_sink_count               = 0;
    s_last_publish_ok          = false;
    s_last_publish_ms          = 0;
    s_published_ever           = false;
    s_paused                   = false;
    s_payload_extender_count   = 0;
    s_payload_hwm_warned       = false;
    // Reset runtime config to defaults.
    s_interval_ms              = (uint32_t)CONFIG_BB_PUB_INTERVAL_MS;
    s_enabled                  = 1;
    s_config_loaded            = true;   /* bypass NVS for tests */
    s_interval_apply_hook      = NULL;
    s_exclusive_holder         = NULL;
    // Re-initialise the tick lock so any lock state from a prior test is cleared.
    pthread_mutex_destroy(&s_tick_lock);
    pthread_mutex_init(&s_tick_lock, NULL);
}
#endif
