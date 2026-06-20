// bb_net_health — pure link-health classifier + ESP-IDF observability satellite.
//
// The classifier (bb_net_health_eval) is a pure, ESP-IDF-free function that
// takes a snapshot of WiFi RSSI and MQTT state and produces a health bucket
// with hysteresis.  It is host-testable with 100% branch coverage.
//
// The ESP-IDF glue (bb_net_health_register_health, bb_net_health_attach_sse)
// lives in platform/espidf/bb_net_health/bb_net_health.c and wires the pure
// classifier to /api/health and a retained "net.health" SSE topic.
//
// Call order (ESP-IDF side):
//   bb_net_health_register_health() — before bb_http_server_start (before
//     the health section table is frozen).
//   bb_net_health_attach_sse()      — in the regular-tier init (after
//     bb_event_routes is initialised).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// RSSI bucket thresholds (compile-time overridable).
// ---------------------------------------------------------------------------

#ifndef BB_NET_HEALTH_RSSI_GOOD
#define BB_NET_HEALTH_RSSI_GOOD     (-67)  // rssi >= -67 → GOOD
#endif

#ifndef BB_NET_HEALTH_RSSI_MARGINAL_LO
#define BB_NET_HEALTH_RSSI_MARGINAL_LO  (-75)  // -75 <= rssi <= -68 → MARGINAL
#endif

// Hysteresis sample counts
#ifndef BB_NET_HEALTH_HYST_DOWN
#define BB_NET_HEALTH_HYST_DOWN  3  // consecutive worse-bucket samples before downgrade
#endif

#ifndef BB_NET_HEALTH_HYST_UP
#define BB_NET_HEALTH_HYST_UP    3  // consecutive better-bucket samples before upgrade
#endif

// ---------------------------------------------------------------------------
// Input / output / state types
// ---------------------------------------------------------------------------

/**
 * Snapshot of network inputs fed to the classifier each evaluation cycle.
 */
typedef struct {
    int8_t   rssi;                  // current WiFi RSSI (dBm)
    bool     mqtt_connected;        // true when MQTT broker is reachable
    uint32_t mqtt_reconnect_count;  // cumulative reconnect count
    uint32_t disc_age_s;            // seconds since last MQTT disconnect (0 = connected)
} bb_net_health_input_t;

/**
 * Ordered health state returned by the classifier.
 * GOOD > MARGINAL > POOR in signal quality.
 */
typedef enum {
    BB_NET_STATE_GOOD     = 0,
    BB_NET_STATE_MARGINAL = 1,
    BB_NET_STATE_POOR     = 2,
} bb_net_state_t;

/**
 * Opaque hysteresis state held by the caller across evaluations.
 * Callers allocate this (typically as a static module variable) and
 * pass it to every bb_net_health_eval call.  Zero-init is valid.
 */
typedef struct {
    bb_net_state_t current_state;       // current published bucket
    int            down_count;          // consecutive worse-bucket samples
    int            up_count;            // consecutive better-bucket samples
    uint32_t       last_reconnect_count; // reconnect_count seen last eval
    // Adaptive-backoff state (used by commit 4 throttle decision)
    int            sustained_poor_count; // consecutive POOR samples (not reset on GOOD)
    bool           throttled;            // true while bb_pub is slowed down
    // Cold-start seeding: first eval bypasses hysteresis to report reality immediately.
    bool           initialized;          // false until first bb_net_health_eval call
} bb_net_health_state_t;

/**
 * Result produced by the classifier on each call.
 */
typedef struct {
    bb_net_state_t state;        // classified bucket after hysteresis
    bool           early_warning; // true when a problem is imminent or ongoing
} bb_net_health_output_t;

// ---------------------------------------------------------------------------
// Pure classifier
// ---------------------------------------------------------------------------

/**
 * Evaluate link health given the current input snapshot, updating hysteresis
 * state and producing an output.
 *
 * Thread-safety: the caller owns `st`; no internal locking.  On a single-
 * evaluator design (e.g. a periodic timer callback) this is always safe.
 *
 * @param st   Hysteresis state (persistent across calls; zero-init for first call).
 * @param in   Current network input snapshot.
 * @param out  Receives the updated health bucket and early_warning flag.
 */
void bb_net_health_eval(bb_net_health_state_t       *st,
                        const bb_net_health_input_t *in,
                        bb_net_health_output_t      *out);

/**
 * Return a static string for a bb_net_state_t value.
 * "good", "marginal", or "poor".  Never returns NULL.
 */
const char *bb_net_state_str(bb_net_state_t state);

// ---------------------------------------------------------------------------
// Adaptive-backoff throttle decision (pure, host-testable; used by commit 4)
// ---------------------------------------------------------------------------

/**
 * Decide whether to throttle bb_pub based on the hysteresis state and the
 * BB_PUB_ADAPTIVE_BACKOFF Kconfig gate.
 *
 * - Returns true  (start throttle) when poor-count >= threshold and not already
 *   throttled.
 * - Returns false (stop throttle)  when state is GOOD or MARGINAL and currently
 *   throttled.
 * - Returns current throttled state otherwise (no change).
 *
 * Updates st->throttled in place.
 *
 * @param st        Hysteresis state (must have been updated by bb_net_health_eval).
 * @param threshold Consecutive POOR samples before throttling (BB_PUB_ADAPTIVE_SAMPLES).
 * @return true = throttling active after this call; false = not throttling.
 */
bool bb_net_health_throttle_decision(bb_net_health_state_t *st, int threshold);

// ---------------------------------------------------------------------------
// ESP-IDF glue (declared here; implemented in platform/espidf/bb_net_health/)
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM

/**
 * Register a /api/health section named "net" that emits:
 *   { "rssi": int, "mqtt_connected": bool, "mqtt_reconnect_count": uint,
 *     "last_disconnect_reason": uint, "disc_age_s": uint,
 *     "state": string, "early_warning": bool }
 *
 * Must be called before bb_http_server_start (before the health section
 * table is frozen).
 */
void bb_net_health_register_health(void);

/**
 * Attach the "net.health" retained SSE topic and start the 5-second
 * evaluation timer.
 *
 * Must be called in the regular-tier init (after bb_event_routes_init).
 * Publishes an initial snapshot immediately so SSE clients connecting
 * before the first tick receive current state.
 */
bb_err_t bb_net_health_attach_sse(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
