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
#include "bb_core.h"
#include "bb_json.h"

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
} bb_pub_sink_t;

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

/**
 * Register a telemetry source. `subtopic` becomes the trailing component of
 * the publish topic: "<prefix>/<hostname>/<subtopic>".
 *
 * When the source registry is at CONFIG_BB_PUB_MAX_SOURCES - 1 entries (the
 * high-watermark), a bb_log_w is emitted once. Attempting to register past
 * CONFIG_BB_PUB_MAX_SOURCES returns BB_ERR_NO_SPACE.
 */
bb_err_t bb_pub_register_source(const char *subtopic, bb_pub_sample_fn fn, void *ctx);

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
// Testing hooks (active when BB_PUB_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_PUB_TESTING

/** Reset source registry, all sinks, and interval/enabled to defaults. */
void bb_pub_test_reset(void);

#endif /* BB_PUB_TESTING */

/**
 * Register a hook called by bb_pub_set_interval_ms after the new value is
 * stored. On ESP-IDF the worker registers this hook at start-up to re-arm the
 * periodic timer; the host build leaves it NULL. Passing NULL clears the hook.
 * This symbol is always available (not gated on BB_PUB_TESTING).
 */
void bb_pub_set_interval_apply_hook(void (*hook)(uint32_t ms));

#ifdef __cplusplus
}
#endif
