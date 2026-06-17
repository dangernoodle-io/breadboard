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
#include "bb_ntp.h"
#include "bb_nv.h"
#include "bb_ring.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// CONFIG_BB_PUB_MAX_SOURCES, CONFIG_BB_PUB_MAX_SINKS,
// CONFIG_BB_PUB_TOPIC_PREFIX, and CONFIG_BB_PUB_INTERVAL_MS are provided by
// Kconfig on ESP-IDF; supply defaults for host builds.
#ifndef CONFIG_BB_PUB_MAX_SOURCES
#define CONFIG_BB_PUB_MAX_SOURCES 8
#endif
#ifndef CONFIG_BB_PUB_MAX_SINKS
#define CONFIG_BB_PUB_MAX_SINKS 4
#endif
#ifndef CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS
#define CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS 4
#endif
#ifndef CONFIG_BB_PUB_SUBTOPIC_MAX
#define CONFIG_BB_PUB_SUBTOPIC_MAX 64
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
    char            subtopic[CONFIG_BB_PUB_SUBTOPIC_MAX];
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

static bb_pub_payload_entry_t s_payload_extenders[CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS];
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
// Store-and-forward buffer (CONFIG_BB_PUB_BUFFER_ENABLE)
// ---------------------------------------------------------------------------
//
// Entry wire format (packed into a single bb_ring push):
//   [topic bytes (NUL-terminated)] [NUL separator] [payload bytes]
//
// The ring max_entry_bytes =
//   BB_PUB_BUFFER_TOPIC_MAX (192) + 1 (NUL sep) + CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES
//
// ring entry `ts`  = capture epoch-ms when NTP-synced, else 0 (uptime-ms
//                    not used here because bb_ring ts is int64 and uptime-ms
//                    would be ambiguous at replay; 0 signals "unknown epoch").
// ring entry `id`  = 0 (unused; reserved for future sequencing).
//
// Replay: on a tick where sink[0] delivers successfully and the ring is
// non-empty, drain oldest-first via peek → inject captured_ms if ts>0 →
// publish to sink[0] → pop on success, stop on failure.
//
// Only sink[0] is used for capture and replay to avoid double-buffering
// the legacy fan-out set.

#if CONFIG_BB_PUB_BUFFER_ENABLE

#ifndef CONFIG_BB_PUB_BUFFER_MAX_ENTRIES
#define CONFIG_BB_PUB_BUFFER_MAX_ENTRIES 16
#endif
#ifndef CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES
#define CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES 256
#endif

// Topic budget: tunable via CONFIG_BB_PUB_BUFFER_TOPIC_MAX (Kconfig).
// Must accommodate CONFIG_BB_PUB_TOPIC_PREFIX + hostname + CONFIG_BB_PUB_SUBTOPIC_MAX + 2 seps + NUL.
#ifndef CONFIG_BB_PUB_BUFFER_TOPIC_MAX
#define CONFIG_BB_PUB_BUFFER_TOPIC_MAX 192
#endif
#define BB_PUB_BUFFER_TOPIC_MAX CONFIG_BB_PUB_BUFFER_TOPIC_MAX
// Total per-entry capacity.
#define BB_PUB_BUFFER_ENTRY_MAX \
    (BB_PUB_BUFFER_TOPIC_MAX + 1 + CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES)

static bb_ring_t s_buffer = NULL;

// Runtime toggle for always-on mode (mirrors CONFIG_BB_PUB_BUFFER_ALWAYS).
// Under BB_PUB_TESTING a test seam overrides the compile-time default so both
// modes can be exercised in the same binary.
#ifdef BB_PUB_TESTING
static int s_buffer_always = -1;   /* -1 = use compile-time default */

void bb_pub_test_set_buffer_always(bool always_on)
{
    s_buffer_always = always_on ? 1 : 0;
}

static bool buffer_always_on(void)
{
    if (s_buffer_always >= 0) return s_buffer_always != 0;
#if CONFIG_BB_PUB_BUFFER_ALWAYS
    return true;
#else
    return false;
#endif
}
#else
static bool buffer_always_on(void)
{
#if CONFIG_BB_PUB_BUFFER_ALWAYS
    return true;
#else
    return false;
#endif
}
#endif /* BB_PUB_TESTING */

// Lazily create the ring on first enqueue.
static bb_ring_t buffer_get(void)
{
    if (!s_buffer) {
        bb_err_t err = bb_ring_create(
            (size_t)CONFIG_BB_PUB_BUFFER_MAX_ENTRIES,
            (size_t)BB_PUB_BUFFER_ENTRY_MAX,
            &s_buffer);
        if (err != BB_OK || !s_buffer) {
            bb_log_w(TAG, "store-and-forward: ring create failed (%d)", err);
            s_buffer = NULL;
        }
    }
    return s_buffer;
}

// Eagerly allocate the ring for always-on mode.
// Called once at init (espidf start) when CONFIG_BB_PUB_BUFFER_ALWAYS=y so
// the ring exists from boot.  Idempotent if the ring already exists.
// In tests the ring is also lazily created by buffer_capture, so this is
// only strictly needed on device to satisfy the standing-cost guarantee.
void bb_pub_buffer_init_eager(void)
{
    if (buffer_always_on()) {
        buffer_get();   /* allocate now; logs warning on failure */
    }
}

// Capture epoch accessor.
// Returns the current wall-clock epoch in milliseconds when NTP is synced,
// otherwise 0. A ts==0 in the ring means "epoch unknown at capture time";
// the receiver uses arrival time instead of a backdated point.
#ifdef BB_PUB_TESTING
// Test hook: override synced epoch for deterministic tests.
static int64_t s_test_epoch_ms = -1;   /* -1 = not overridden */

void bb_pub_test_set_synced_epoch_ms(int64_t epoch_ms)
{
    s_test_epoch_ms = epoch_ms;
}

static int64_t capture_epoch_ms(void)
{
    if (s_test_epoch_ms >= 0) {
        return s_test_epoch_ms;
    }
    if (bb_ntp_is_synced()) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    return 0;
}
#else
static int64_t capture_epoch_ms(void)
{
    if (!bb_ntp_is_synced()) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif /* BB_PUB_TESTING */

// Push topic+payload into the ring.
// Called under s_tick_lock.
// Returns true if the entry was successfully enqueued, false if it was rejected
// (topic too long or entry too large for a ring slot).  Callers in always-on
// mode use the return value to fall back to a direct publish on rejection so
// oversized entries are not silently dropped.
static bool buffer_capture(const char *topic, const char *payload, int payload_len)
{
    bb_ring_t r = buffer_get();
    if (!r) return false;

    size_t topic_len = strlen(topic);
    if (topic_len >= BB_PUB_BUFFER_TOPIC_MAX) {
        bb_log_d(TAG, "buffer: topic too long (%zu), not buffered", topic_len);
        return false;
    }

    // Pack: topic + NUL + payload
    size_t entry_len = topic_len + 1 + (size_t)payload_len;
    if (entry_len > BB_PUB_BUFFER_ENTRY_MAX) {
        bb_log_d(TAG, "buffer: entry too large (%zu > %zu), not buffered",
                 entry_len, (size_t)BB_PUB_BUFFER_ENTRY_MAX);
        return false;
    }

    // Stack-allocate entry (max 192+1+256 = 449 bytes; safe on any stack).
    char entry[BB_PUB_BUFFER_ENTRY_MAX];
    memcpy(entry, topic, topic_len);
    entry[topic_len] = '\0';
    if (payload_len > 0) {
        memcpy(entry + topic_len + 1, payload, (size_t)payload_len);
    }

    int64_t ts = capture_epoch_ms();
    bb_err_t err = bb_ring_push(r, entry, entry_len, ts, 0);
    if (err != BB_OK) {
        bb_log_w(TAG, "buffer: ring push failed (%d)", err);
    } else {
        bb_log_d(TAG, "buffer: captured '%s' (epoch_ms=%lld, ring=%zu)",
                 topic, (long long)ts, bb_ring_count(r));
    }
    return true;
}

// Splice captured_ms into a JSON payload when ts > 0.
// Inserts `"captured_ms":<ts>,` right after the opening '{'.
// Returns a heap-allocated string that the caller must free with free(), or
// NULL if no injection needed (ts == 0) — caller uses original payload then.
// Also returns NULL on alloc failure — caller falls back to original.
static char *inject_captured_ms(const char *payload, int payload_len, int64_t ts)
{
    if (ts <= 0) return NULL;
    if (payload_len < 2 || payload[0] != '{') return NULL;

    // Build: {"captured_ms":<ts>,<rest after '{'>
    // field = "captured_ms":NNNNNNNNNNNNNN,  (max 14 digits + overhead = ~22 chars)
    char field[40];
    int field_len = snprintf(field, sizeof(field), "\"captured_ms\":%lld,", (long long)ts);
    if (field_len < 0 || (size_t)field_len >= sizeof(field)) return NULL;

    int out_len = 1 + field_len + payload_len - 1;  /* '{' + field + rest */
    char *out = malloc((size_t)out_len + 1);
    if (!out) return NULL;

    out[0] = '{';
    memcpy(out + 1, field, (size_t)field_len);
    // rest = payload[1..payload_len-1]
    memcpy(out + 1 + field_len, payload + 1, (size_t)(payload_len - 1));
    out[out_len] = '\0';
    return out;
}

// Replay buffered entries to sink[0].
// Called under s_tick_lock.  In on-failure mode (always_on=false) this is
// called only after a successful live-publish tick.  In always-on mode it is
// called unconditionally every tick.
//
// now_epoch_ms     — current wall-clock epoch in ms (0 if NTP not synced).
// delay_threshold_ms — when > 0, captured_ms is injected ONLY if the entry's
//                    age (now - entry_ts) exceeds this threshold. When 0,
//                    captured_ms is injected for any entry with entry_ts > 0
//                    (on-failure mode: every buffered entry is by definition
//                    delayed, so no age gate is needed).
//
// In always-on mode the caller passes 1.5 × interval_ms so that fresh entries
// drained the same tick they were enqueued carry no captured_ms (age ≈ 0),
// while entries that survived a real outage across ticks do carry it.
// In on-failure mode the caller passes 0 to preserve the prior behavior.
static void buffer_replay(const bb_pub_sink_t *sink,
                          int64_t now_epoch_ms, int64_t delay_threshold_ms)
{
    bb_ring_t r = s_buffer;
    if (!r || bb_ring_count(r) == 0) return;

    // Replay buffer: peek content size, alloc scratch, deliver, pop on success.
    static char s_replay_buf[BB_PUB_BUFFER_ENTRY_MAX + 1];

    for (;;) {
        size_t   entry_len = 0;
        int64_t  entry_ts  = 0;
        uint32_t entry_id  = 0;

        bb_err_t err = bb_ring_peek_oldest(r,
                                           s_replay_buf, sizeof(s_replay_buf) - 1,
                                           &entry_len, &entry_ts, &entry_id);
        if (err == BB_ERR_NOT_FOUND) break;  /* ring empty */
        if (err != BB_OK) {
            bb_log_w(TAG, "buffer: peek failed (%d), stopping replay", err);
            break;
        }

        // Parse topic (NUL-terminated at s_replay_buf[topic_len]).
        s_replay_buf[entry_len] = '\0';
        const char *topic   = s_replay_buf;
        size_t topic_len    = strlen(topic);
        const char *payload = (topic_len + 1 < entry_len)
                              ? s_replay_buf + topic_len + 1
                              : "";
        int payload_len     = (int)(entry_len - topic_len - 1);
        if (payload_len < 0) payload_len = 0;

        // Determine whether to inject captured_ms.
        //   - delay_threshold_ms == 0 (on-failure mode): inject whenever
        //     entry_ts > 0, regardless of age (every buffered entry is
        //     delayed by definition — it failed at least one prior tick).
        //   - delay_threshold_ms > 0 (always-on mode): inject only when the
        //     entry is genuinely old; same-tick fresh entries are not stamped.
        int64_t inject_ts = 0;
        if (entry_ts > 0 && now_epoch_ms > 0) {
            if (delay_threshold_ms == 0) {
                inject_ts = entry_ts;   /* on-failure: always stamp */
            } else {
                int64_t age_ms = now_epoch_ms - entry_ts;
                if (age_ms > delay_threshold_ms) {
                    inject_ts = entry_ts;
                }
            }
        }

        char *patched = inject_captured_ms(payload, payload_len, inject_ts);
        const char *pub_payload = patched ? patched : payload;
        int         pub_len     = patched ? (int)strlen(patched) : payload_len;

        bb_err_t deliver = sink->publish(sink->ctx, topic, pub_payload, pub_len);
        free(patched);   /* NULL-safe */

        if (deliver != BB_OK) {
            bb_log_w(TAG, "buffer: replay delivery failed for '%s': %d", topic, deliver);
            break;   /* sink still down; leave rest in ring */
        }

        bb_log_d(TAG, "buffer: replayed '%s' (epoch_ms=%lld, inject_ts=%lld)",
                 topic, (long long)entry_ts, (long long)inject_ts);
        bb_ring_pop_oldest(r);
    }
}

#else  /* !CONFIG_BB_PUB_BUFFER_ENABLE */

// Stubs so callers always link cleanly when the buffer feature is compiled out.
#ifdef BB_PUB_TESTING
void bb_pub_test_set_synced_epoch_ms(int64_t epoch_ms) { (void)epoch_ms; }
void bb_pub_test_set_buffer_always(bool always_on)     { (void)always_on; }
#endif
void bb_pub_buffer_init_eager(void) {}

#endif /* CONFIG_BB_PUB_BUFFER_ENABLE */

// ---------------------------------------------------------------------------
// Public API — buffer stats
// ---------------------------------------------------------------------------

void bb_pub_buffer_stats(bb_pub_buffer_stats_t *out)
{
    if (!out) return;
#if CONFIG_BB_PUB_BUFFER_ENABLE
    bb_ring_t r = s_buffer;
    if (r) {
        out->count     = bb_ring_count(r);
        out->dropped   = bb_ring_dropped(r);
        out->truncated = bb_ring_truncated(r);
    } else {
        out->count     = 0;
        out->dropped   = 0;
        out->truncated = 0;
    }
#else
    out->count     = 0;
    out->dropped   = 0;
    out->truncated = 0;
#endif
}

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

    if (s_payload_extender_count >= CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS) {
        bb_log_w(TAG, "payload extender registry full (%d/%d)",
                 s_payload_extender_count, CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS);
        return BB_ERR_NO_SPACE;
    }

    // High-watermark warning at cap-1.
    if (!s_payload_hwm_warned &&
        s_payload_extender_count == CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS - 1) {
        bb_log_w(TAG, "payload extender registry at high-watermark (%d/%d)",
                 s_payload_extender_count, CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS);
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
#if CONFIG_BB_PUB_BUFFER_ENABLE
    // Capture current wall-clock epoch once (0 if NTP not synced).
    int64_t tick_epoch_ms = capture_epoch_ms();
#endif

    const char *hostname = bb_nv_config_hostname();
    if (!hostname || hostname[0] == '\0') {
        hostname = "device";
    }

    // +prefix_len+1+hostname_len+1+subtopic_len+1 — using generous fixed size
    // that covers CONFIG_BB_PUB_SUBTOPIC_MAX plus typical prefix/hostname.
    char topic[CONFIG_BB_PUB_SUBTOPIC_MAX + 128];

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

#if CONFIG_BB_PUB_BUFFER_ENABLE
            // Always-on mode: prefer enqueuing into the ring so the entry is
            // drained in order after the source loop.  If the entry is too
            // large for a ring slot, buffer_capture returns false — fall
            // through to a direct publish so the live data isn't lost (it just
            // won't be buffered for replay).
            if (si == 0 && buffer_always_on()) {
                if (buffer_capture(topic, json, json_len)) {
                    bb_json_free_str(json);
                    continue;   /* enqueued; drained after the source loop */
                }
                // Oversized: can't buffer it — publish directly so the live
                // data isn't lost.  Falls through to sk->publish below.
                // Note: last_publish_ok reflects ring-drain status, so this
                // direct publish does not affect drained semantics.
                bb_log_d(TAG, "buffer: oversized entry published directly (not buffered for replay)");
            }
#endif

            bb_err_t err = sk->publish(sk->ctx, topic, json, json_len);

            if (err != BB_OK) {
                bb_log_w(TAG, "sink[%d] publish failed for '%s': %d",
                         si, src->subtopic, err);
                tick_all_ok = false;
#if CONFIG_BB_PUB_BUFFER_ENABLE
                // On-failure mode: capture to ring on sink[0] failure only.
                // Best-effort; ignore the bool return (oversized → silently skipped).
                if (si == 0) {
                    (void)buffer_capture(topic, json, json_len);
                }
#endif
            }

            bb_json_free_str(json);
        }

        bb_json_free(obj);
        tick_published = true;
    }

#if CONFIG_BB_PUB_BUFFER_ENABLE
    uint32_t interval_ms = bb_pub_get_interval_ms();

    if (buffer_always_on()) {
        // Always-on mode: drain ring every tick regardless of tick_all_ok.
        // The ring was fed by the enqueue path above for every source.
        // Drain happens after all sources so ordering is preserved.
        if (s_sink_count > 0) {
            // Delay threshold: 1.5 × interval.  Fresh entries (age ≈ 0) will
            // not exceed this → no captured_ms on healthy same-tick points.
            int64_t threshold_ms = (int64_t)interval_ms + (int64_t)interval_ms / 2;
            buffer_replay(&s_sinks[0], tick_epoch_ms, threshold_ms);
        }
        // last_publish_ok = did the ring drain fully this tick?
        // A full drain means s_buffer is empty after replay.
        if (tick_published) {
            s_last_publish_ms = ts_ms;
            s_published_ever  = true;
            bool drained = (s_buffer == NULL || bb_ring_count(s_buffer) == 0);
            s_last_publish_ok = drained;
        }
    } else {
        // On-failure mode (default): update status first, then replay on
        // success. Unchanged behavior.
        if (tick_published) {
            s_last_publish_ms  = ts_ms;
            s_published_ever   = true;
            s_last_publish_ok  = tick_all_ok;
        }
        // Replay buffered entries when sink[0] succeeded this tick and the
        // ring is non-empty.  Replay happens after all live sources so it
        // does not interleave with the current tick's live data.
        // Pass delay_threshold_ms=0: on-failure mode stamps any ts>0 entry
        // unconditionally (every buffered entry is delayed by definition).
        if (tick_all_ok && tick_published && s_sink_count > 0) {
            buffer_replay(&s_sinks[0], tick_epoch_ms, 0);
        }
    }
#else
    // Update status only when at least one source was published this tick.
    if (tick_published) {
        s_last_publish_ms  = ts_ms;
        s_published_ever   = true;
        s_last_publish_ok  = tick_all_ok;
    }
#endif

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
#if CONFIG_BB_PUB_BUFFER_ENABLE
    if (s_buffer) {
        bb_ring_clear(s_buffer);
    }
    s_test_epoch_ms   = -1;
    s_buffer_always   = -1;   /* revert to compile-time default */
#endif
    // Re-initialise the tick lock so any lock state from a prior test is cleared.
    pthread_mutex_destroy(&s_tick_lock);
    pthread_mutex_init(&s_tick_lock, NULL);
}
#endif
