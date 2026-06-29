// bb_pub — transport-agnostic telemetry publisher core.
//
// Maintains a registry of sample functions (sources) and a set of publish
// sinks (fan-out). On each tick, each registered source is called, its JSON
// object is serialized ONCE, and the result is forwarded to every registered
// sink. Sinks are deliberately decoupled from transport — see bb_sink_mqtt for
// the MQTT adapter and bb_sink_http for the HTTP adapter.
//
// Thread-safety: bb_pub_set_sink, bb_pub_add_sink, bb_pub_clear_sinks, and
// bb_pub_register_source must be called before any concurrent tick.
// bb_pub_tick_once is NOT reentrant; call it from a single worker task (or
// sequentially in tests).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "bb_cache.h"
#include "bb_core.h"
#include "bb_json.h"

// ---------------------------------------------------------------------------
// Telemetry source registry (gather-into-cache model)
// ---------------------------------------------------------------------------

/**
 * Gather function: populate *snap_buf with the current telemetry snapshot.
 * snap_buf is zeroed before the call; gather fills it and stamps any internal
 * timestamp fields. Return true to commit the snapshot to bb_cache and fan out
 * this tick; false to skip (e.g. hardware not ready).
 * IMPORTANT: gather is called under s_tick_lock — no blocking I/O.
 */
typedef bool (*bb_pub_gather_fn)(void *snap_buf, void *ctx);

/** Flags that control how a telemetry source is fan-out delivered. */
typedef uint32_t bb_pub_telemetry_flags_t;

/** Post the snapshot to the SSE event bus via bb_cache_post each tick. */
#define BB_PUB_TELEM_SSE   (1u << 0)
/** Deliver the snapshot to all registered sinks via bb_pub_publish_from_cache. */
#define BB_PUB_TELEM_SINKS (1u << 1)

/**
 * Configuration for a telemetry source registered via bb_pub_register_telemetry.
 * All fields except ctx are required (must be non-NULL / non-zero).
 */
typedef struct {
    const char              *topic;     /**< Subtopic name (e.g. "wifi"). Stable pointer. */
    bb_pub_gather_fn         gather;    /**< Gather fn; called under s_tick_lock. */
    bb_cache_serialize_fn    serialize; /**< Serializer fn forwarded to bb_cache_register_ex. */
    size_t                   snap_size; /**< sizeof the snapshot struct. */
    bb_pub_telemetry_flags_t flags;     /**< BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS. */
    void                    *ctx;       /**< Opaque context; passed to gather. */
} bb_pub_telemetry_cfg_t;

/**
 * Register a telemetry source in the gather-into-cache pipeline.
 *
 * Per-registration actions:
 *   1. Append to the internal telem-sources table.
 *   2. Call bb_cache_register_ex with BB_CACHE_FLAG_SSE when cfg->flags has
 *      BB_PUB_TELEM_SSE; BB_CACHE_FLAG_NONE otherwise.
 *   3. Register a back-compat adapter sample_fn via bb_pub_register_source so
 *      the source appears in bb_pub_source_count/info and /api/telemetry/metrics.
 *      The adapter reads from bb_cache (never re-gathers).
 *
 * Returns BB_ERR_INVALID_ARG if cfg or any required field is NULL/0.
 * Returns BB_ERR_NO_SPACE if the source registry or telem table is full, or if
 * cfg->snap_size exceeds CONFIG_BB_PUB_TELEM_SNAP_MAX.
 */
bb_err_t bb_pub_register_telemetry(const bb_pub_telemetry_cfg_t *cfg);

// ---------------------------------------------------------------------------
// Per-source tags
// ---------------------------------------------------------------------------

/** Maximum tags per source (not Kconfig). */
#define BB_PUB_MAX_TAGS_PER_SOURCE 8
/** Maximum length of a single tag string (not including NUL). */
#define BB_PUB_TAG_MAX 32

// ---------------------------------------------------------------------------
// Per-sink subscription filter
// ---------------------------------------------------------------------------

/**
 * Optional subscription predicate on a sink.
 * Returns true to receive this source's payload; false to skip it.
 * NULL = receive ALL sources (default, back-compat).
 */
typedef bool (*bb_pub_subscribe_fn)(const char *subtopic,
                                    const char *const *tags, int ntags,
                                    void *ctx);

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Sink interface
// ---------------------------------------------------------------------------

/**
 * Publish function: delivers a serialized JSON payload to `topic`.
 * `len` is the byte length of `payload` (not including the NUL terminator).
 * Return BB_OK on success; any other value is logged but does not abort the
 * tick cycle.
 */
typedef bb_err_t (*bb_pub_publish_fn)(void *ctx, const char *topic,
                                      const char *payload, int len);

typedef struct {
    bb_pub_publish_fn publish;
    void             *ctx;
    /**
     * Optional transport label injected as "transport" into every payload
     * delivered by this sink. Typical values: "mqtt", "http". NULL = omit
     * the field. Must point to a string with lifetime >= the sink lifetime.
     */
    const char       *transport;
    /**
     * Whether TLS is active on this sink's transport. Injected as "tls"
     * (boolean) into every payload when transport is non-NULL. Determined
     * at wire time (bb_sink_mqtt / bb_sink_http call) rather than per-publish,
     * because transport config is boot-fixed under mutual-exclusion and
     * reboot-to-switch semantics — the value is correct for the entire boot.
     */
    bool              tls;
    bb_pub_subscribe_fn subscribe;     /**< Optional subscription filter; NULL = all. */
    void               *subscribe_ctx; /**< Passed to subscribe(); may be NULL. */
} bb_pub_sink_t;

// ---------------------------------------------------------------------------
// Subscription filter helper
// ---------------------------------------------------------------------------

/**
 * Allow-list filter for per-sink subscription.
 * OR logic between subtopics and tags independently:
 * - If subtopics is non-NULL: matches when subtopic is in the list.
 * - If tags is non-NULL: matches when any source tag is in the list.
 * - If both non-NULL: matches when EITHER condition is true.
 * - If both NULL: always matches (pass-all).
 */
typedef struct {
    const char *const *subtopics;  /**< Allow-list of subtopic names; NULL = any. */
    int                nsubtopics;
    const char *const *tags;       /**< Allow-list of tag strings; NULL = any. */
    int                ntags;
} bb_pub_subscription_t;

/**
 * Pure match function. Returns true when subtopic or any source tag matches
 * the allow-list (OR logic per list; both NULL = match all).
 */
bool bb_pub_subscription_match(const bb_pub_subscription_t *sub,
                                const char *subtopic,
                                const char *const *tags, int ntags);

/**
 * Stateless predicate suitable for use as bb_pub_sink_t.subscribe.
 * ctx must be a const bb_pub_subscription_t *.
 */
bool bb_pub_subscription_predicate(const char *subtopic,
                                   const char *const *tags, int ntags,
                                   void *ctx);

// ---------------------------------------------------------------------------
// Source interface
// ---------------------------------------------------------------------------

/**
 * Sample function: populate `obj` with fields for this source.
 * Return true to publish this object on the current tick; false to skip.
 * The caller always frees `obj` after the call, regardless of the return
 * value — do NOT free it inside the callback.
 */
typedef bool (*bb_pub_sample_fn)(bb_json_t obj, void *ctx);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Set (or replace) the active sink — back-compat single-sink API.
 * Clears all previously registered sinks, then adds this one.
 * Pass NULL sink or NULL sink->publish to clear all sinks; tick is then a
 * no-op (same as before). Existing single-sink callers are unaffected.
 */
bb_err_t bb_pub_set_sink(const bb_pub_sink_t *sink);

/**
 * Add a sink to the fan-out set. All registered sinks receive every published
 * payload on each tick. A sink returning non-BB_OK is logged (bb_log_w) but
 * does not abort delivery to the remaining sinks or sources.
 *
 * Returns BB_ERR_NO_SPACE when the sink array is full
 * (CONFIG_BB_PUB_MAX_SINKS). Returns BB_ERR_INVALID_ARG if sink or
 * sink->publish is NULL.
 */
bb_err_t bb_pub_add_sink(const bb_pub_sink_t *sink);

/**
 * Remove all registered sinks. After this call, tick is a no-op until a sink
 * is registered again.
 */
void bb_pub_clear_sinks(void);

// ---------------------------------------------------------------------------
// Payload-extender interface
// ---------------------------------------------------------------------------

/**
 * Payload extender callback. Called for EACH source's JSON object every tick,
 * AFTER its sample_fn fills it and the shared `ts` is injected, BEFORE
 * serialize. `obj` is MUTABLE — add or alter fields. `source` is the source
 * subtopic name. Runs only when the tick actually publishes (skipped when
 * paused or disabled).
 */
typedef void (*bb_pub_payload_fn)(bb_json_t obj, const char *source, void *ctx);

/**
 * Register a payload extender. Extenders are invoked in registration order
 * on every published message. A registered extender sees the full JSON object
 * (including the ts field) and may add or alter any field.
 *
 * Cap: BB_PUB_MAX_PAYLOAD_EXTENDERS (default 4). When the registry is at
 * cap-1 (high-watermark), a bb_log_w is emitted once. Attempting to register
 * past the cap returns BB_ERR_NO_SPACE.
 *
 * Returns BB_ERR_INVALID_ARG if fn is NULL.
 */
bb_err_t bb_pub_register_payload_extender(bb_pub_payload_fn fn, void *ctx);

// ---------------------------------------------------------------------------
// Source interface
// ---------------------------------------------------------------------------

/**
 * Register a telemetry source. `subtopic` becomes the trailing component of
 * the publish topic: "<prefix>/<hostname>/<subtopic>".
 *
 * When the source registry is at CONFIG_BB_PUB_MAX_SOURCES - 1 entries (the
 * high-watermark), a bb_log_w is emitted once. Attempting to register past
 * CONFIG_BB_PUB_MAX_SOURCES returns BB_ERR_NO_SPACE.
 */
bb_err_t bb_pub_register_source(const char *subtopic, bb_pub_sample_fn fn, void *ctx);

/**
 * Register a telemetry source with optional tags.
 * Tags are copied (up to BB_PUB_MAX_TAGS_PER_SOURCE, each up to BB_PUB_TAG_MAX chars).
 * NULL tags / 0 ntags = no tags (equivalent to bb_pub_register_source).
 */
bb_err_t bb_pub_register_source_ex(const char *subtopic, bb_pub_sample_fn fn, void *ctx,
                                    const char *const *tags, int ntags);

// ---------------------------------------------------------------------------
// Source enumeration (used by bb_telemetry for GET /api/telemetry/metrics)
// ---------------------------------------------------------------------------

/**
 * Return the number of registered sources.
 */
int bb_pub_source_count(void);

/**
 * Return metadata for source at index i.
 * All out-params are optional (pass NULL to skip).
 * Returns BB_ERR_INVALID_ARG if i is out of range.
 */
bb_err_t bb_pub_source_info(int i, const char **subtopic, bb_pub_sample_fn *fn,
                             void **ctx, uint32_t *last_sample_ms, bool *sampled_ever);

/**
 * Return metadata for sink at index i.
 * out_transport may be NULL (sink has no transport label).
 * All out-params are optional (pass NULL to skip).
 * Returns BB_ERR_INVALID_ARG if i is out of range [0, sink_count).
 */
bb_err_t bb_pub_sink_info(int i, const char **out_transport, bool *out_tls);

/**
 * Extended source info; also returns tags and ntags. All out-params optional.
 * tags points into internal storage (valid until reset).
 */
bb_err_t bb_pub_source_info_ex(int i, const char **subtopic, bb_pub_sample_fn *fn,
                                void **ctx, uint32_t *last_sample_ms, bool *sampled_ever,
                                const char *const **tags, int *ntags);

/**
 * Find the source registered under `subtopic`, call its sample_fn(obj, ctx),
 * and return its bool. Returns false if no source matches.
 * The caller owns `obj` and must free it — same contract as sample_fn.
 */
bool bb_pub_sample_into(const char *subtopic, bb_json_t obj);

/**
 * True when always-on buffering is enabled and the ring entry cap is smaller
 * than the registered source count (data loss risk on next tick).
 * Always false when CONFIG_BB_PUB_BUFFER_ENABLE is off.
 */
bool bb_pub_ring_undersized(void);

// ---------------------------------------------------------------------------
// Prometheus metric-name prefix (consumed by bb_telemetry's GET /api/telemetry/metrics)
// ---------------------------------------------------------------------------

/**
 * Set the Prometheus metric name prefix at runtime. The string is copied into
 * a static buffer (max 64 bytes including NUL). Defaults to
 * CONFIG_BB_METRICS_PREFIX. NULL is ignored.
 * TaipanMiner calls bb_pub_set_metrics_prefix("taipanminer") before init.
 */
void bb_pub_set_metrics_prefix(const char *prefix);

/**
 * Return the current Prometheus metric name prefix. Never NULL; defaults to
 * the CONFIG_BB_METRICS_PREFIX value.
 */
const char *bb_pub_metrics_prefix(void);

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

/**
 * Snapshot of the publisher's runtime state, updated at the end of each tick.
 * Suitable for GET /api/pub and UI health indicators.
 */
typedef struct {
    int      source_count;      /**< Number of registered sources. */
    int      sink_count;        /**< Number of registered sinks. */
    bool     last_publish_ok;   /**< True if the last tick that published
                                     had all sink calls return BB_OK. */
    uint32_t last_publish_ms;   /**< bb_clock_now_ms() at the last tick that
                                     published ≥1 source. 0 = never. */
    bool     published_ever;    /**< True once at least one tick published. */
    /**
     * True when the publisher worker was successfully started (bb_pub_start
     * completed without error). False on builds where AUTOREGISTER=n and
     * bb_pub_start was never called, or when bb_pub_start failed.
     *
     * Use this to distinguish "publisher running" from "publisher compiled in
     * but not started". A reboot will NOT enable the publisher on a build where
     * this is false due to AUTOREGISTER=n — it requires a firmware rebuild.
     */
    bool     available;
} bb_pub_status_t;

/**
 * Copy the current publisher status into `out`. Thread-safety: safe to call
 * from any context; reads file-scope atomics. Always returns BB_OK.
 */
bb_err_t bb_pub_get_status(bb_pub_status_t *out);

// ---------------------------------------------------------------------------
// Runtime configuration — interval and enable toggle
// ---------------------------------------------------------------------------

/**
 * Set the publish interval. Persists to NVS (namespace "bb_pub", key
 * "interval_ms") and, on ESP-IDF, re-arms the periodic timer immediately so
 * the new period takes effect without reboot.
 *
 * Valid range: 1000 ms .. 3 600 000 ms (1 s .. 1 hr). Returns
 * BB_ERR_INVALID_ARG for 0 or any value outside that range.
 *
 * Note: the ESP-IDF timer re-arm path requires on-hardware confirmation; the
 * host build updates the in-RAM value only (no timer to re-arm).
 */
bb_err_t bb_pub_set_interval_ms(uint32_t ms);

/**
 * Return the current effective publish interval in milliseconds. Defaults to
 * CONFIG_BB_PUB_INTERVAL_MS when NVS is empty.
 */
uint32_t bb_pub_get_interval_ms(void);

/**
 * Enable or disable the publisher persistently (NVS namespace "bb_pub", key
 * "enabled"). When disabled, bb_pub_tick_once is a cheap no-op regardless of
 * the pause/resume state. Persists across reboots.
 *
 * DISTINCTION from pause/resume: `enabled` is a persistent config toggle
 * (user-controlled, survives reboot). `pause/resume` is a transient,
 * in-memory gate for short-lived quiescing (e.g. during OTA). Both gates are
 * independent: a tick publishes ONLY when enabled=true AND not paused.
 */
bb_err_t bb_pub_set_enabled(bool en);

/**
 * Return true if the publisher is currently enabled (NVS-persisted flag,
 * default true). Independent from the pause state; see bb_pub_set_enabled.
 */
bool bb_pub_is_enabled(void);

// ---------------------------------------------------------------------------
// Pause / resume  (transient, in-memory — see bb_pub_set_enabled for the
//                  persistent enabled flag)
// ---------------------------------------------------------------------------

/**
 * Pause publishing. While paused, bb_pub_tick_once returns BB_OK immediately
 * without calling any source sample_fn or sink publish. Idempotent.
 * Safe to call from any context (e.g. an OTA pause hook).
 *
 * Gating: a tick publishes only when enabled=true AND not paused.
 */
void bb_pub_pause(void);

/**
 * Resume publishing after a bb_pub_pause call. Idempotent; safe to call when
 * not paused.
 */
void bb_pub_resume(void);

/**
 * Returns true if the publisher is currently paused.
 */
bool bb_pub_is_paused(void);

/**
 * Run one sample+publish cycle synchronously (used by the ESP-IDF worker task
 * and host tests). For each registered source:
 *   1. Allocate a new JSON object.
 *   2. Call sample_fn; if it returns false, skip and free.
 *   3. Inject "ts" field (uptime-ms from bb_clock_now_ms on host; on device
 *      the same uptime-ms value is used — callers that need wall-clock time
 *      should include it themselves using bb_ntp or similar).
 *   4. Serialize to JSON string, call sink->publish, free the string.
 *   5. Free the JSON object.
 *
 * All sources share the same `ts` snapshot taken once at the start of the
 * tick so that a set of related metrics carries a consistent timestamp.
 *
 * Returns BB_OK even when no sink is set or all sources are skipped.
 */
bb_err_t bb_pub_tick_once(void);

// ---------------------------------------------------------------------------
// Store-and-forward buffer observability
// ---------------------------------------------------------------------------

/**
 * Counters from the store-and-forward ring buffer. All fields are zero when
 * CONFIG_BB_PUB_BUFFER_ENABLE is off (compiled-out stub) or when the ring
 * has not yet been created.
 */
typedef struct {
    size_t count;     /**< Current entries waiting for replay. */
    size_t dropped;   /**< Entries evicted due to ring-full overflow. */
    size_t truncated; /**< Push attempts rejected: entry too large for ring. */
} bb_pub_buffer_stats_t;

/**
 * Read current store-and-forward buffer stats. Always succeeds; returns zeros
 * when the feature is compiled out or the ring is not yet initialised.
 */
void bb_pub_buffer_stats(bb_pub_buffer_stats_t *out);

// ---------------------------------------------------------------------------
// Exclusive-sink arbiter
// ---------------------------------------------------------------------------
// Enforces mutual exclusion between telemetry sinks: at most ONE exclusive sink
// may be active at a time. Each sink identifies itself by a stable string ID
// (e.g. "mqtt", "http").
//
// Boot precedence: if NVS is left with two sinks both marked enabled, the first
// sink that calls acquire (MQTT, registered earlier in the PRE_HTTP walk) wins;
// subsequent callers from the conflicting sink receive BB_ERR_CONFLICT and must
// treat themselves as disabled.

/**
 * Try to acquire the exclusive-sink slot for `sink_id`.
 * - If the slot is free: acquired → BB_OK.
 * - If already held by `sink_id`: idempotent → BB_OK.
 * - If held by a different id: → BB_ERR_CONFLICT.
 * `sink_id` must be a stable, non-NULL string.
 */
bb_err_t bb_pub_exclusive_acquire(const char *sink_id);

/**
 * Release the exclusive-sink slot if held by `sink_id`.
 * Calling with an id that does not hold the slot is a safe no-op.
 */
void bb_pub_exclusive_release(const char *sink_id);

// ---------------------------------------------------------------------------
// Testing hooks (active when BB_PUB_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_PUB_TESTING

/** Reset source registry, all sinks, interval/enabled, and exclusive arbiter to defaults. */
void bb_pub_test_reset(void);

/** Reset only the exclusive-sink arbiter (for arbiter-unit tests). */
void bb_pub_exclusive_reset(void);

/**
 * Inject a fake NTP-synced epoch (ms) for buffer tests. Pass 0 to simulate
 * "not yet synced". Only available when BB_PUB_TESTING is defined.
 * Only meaningful when CONFIG_BB_PUB_BUFFER_ENABLE is 1.
 */
void bb_pub_test_set_synced_epoch_ms(int64_t epoch_ms);

/**
 * Override the always-on ring mode at runtime for testing. When true, forces
 * always-on behaviour regardless of CONFIG_BB_PUB_BUFFER_ALWAYS; when false,
 * forces on-failure (default) behaviour. Pass -1 (cast to bool true/false) is
 * not applicable — use bb_pub_test_reset() to revert to compile-time default.
 * Only meaningful when CONFIG_BB_PUB_BUFFER_ENABLE is 1.
 */
void bb_pub_test_set_buffer_always(bool always_on);

#endif /* BB_PUB_TESTING */

/**
 * Eagerly allocate the store-and-forward ring for always-on mode.
 * Called once at init when CONFIG_BB_PUB_BUFFER_ALWAYS=y so the ring exists
 * from boot (standing RAM cost accepted by the caller). Idempotent.
 * On-failure mode (CONFIG_BB_PUB_BUFFER_ALWAYS=n) keeps lazy allocation;
 * calling this function is a no-op in that case.
 * Only meaningful when CONFIG_BB_PUB_BUFFER_ENABLE is 1.
 */
void bb_pub_buffer_init_eager(void);

/**
 * Register a hook called by bb_pub_set_interval_ms after the new value is
 * stored. On ESP-IDF the worker registers this hook at start-up to re-arm the
 * periodic timer; the host build leaves it NULL. Passing NULL clears the hook.
 * This symbol is always available (not gated on BB_PUB_TESTING).
 */
void bb_pub_set_interval_apply_hook(void (*hook)(uint32_t ms));

/**
 * Mark the publisher as successfully started. Called by the ESP-IDF platform
 * backend (bb_pub_espidf.c) after bb_pub_start() completes successfully so
 * bb_pub_get_status().available reflects actual registration state.
 *
 * On host builds / AUTOREGISTER=n builds, this function is never called and
 * available remains false — correctly indicating the periodic worker is absent.
 * Consumers that start the publisher manually (AUTOREGISTER=n) must call this
 * after a successful bb_pub_start() to keep the flag honest.
 *
 * This symbol is always available (not gated on BB_PUB_TESTING).
 */
void bb_pub_mark_started(void);

/**
 * Volatile (RAM-only) interval override — re-arms the periodic timer to `ms`
 * WITHOUT writing NVS and WITHOUT changing the value returned by
 * bb_pub_get_interval_ms() (which continues to return the persisted/configured
 * interval).
 *
 * Use this for transient throttle adjustments (e.g. adaptive backoff during
 * poor link) so that a reboot-while-throttled boots at the configured interval
 * rather than the throttled one.
 *
 * Valid range: 1000 ms .. 3 600 000 ms (same as bb_pub_set_interval_ms).
 * Returns BB_ERR_INVALID_ARG for values outside that range.
 * On ESP-IDF, calls the interval_apply_hook to re-arm the timer immediately.
 * On host (no timer), updates no stored state — purely a timer re-arm.
 */
bb_err_t bb_pub_set_interval_volatile_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
