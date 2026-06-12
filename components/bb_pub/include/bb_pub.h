// bb_pub — transport-agnostic telemetry publisher core.
//
// Maintains a registry of sample functions (sources) and a single publish
// sink. On each tick, each registered source is called, its JSON object
// is serialized, and the result is forwarded to the sink. The sink is
// deliberately decoupled from transport — see bb_pub_mqtt for the MQTT adapter.
//
// Thread-safety: bb_pub_set_sink and bb_pub_register_source must be called
// before any concurrent tick. bb_pub_tick_once is NOT reentrant; call it
// from a single worker task (or sequentially in tests).
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
 * Set (or replace) the active sink. Pass NULL sink or NULL sink->publish to
 * clear. bb_pub_tick_once silently skips publishing when no sink is set.
 */
bb_err_t bb_pub_set_sink(const bb_pub_sink_t *sink);

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

/** Reset source registry and sink to empty state. */
void bb_pub_test_reset(void);

#endif /* BB_PUB_TESTING */

#ifdef __cplusplus
}
#endif
