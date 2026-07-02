// bb_net_health — pure link-health classifier + ESP-IDF observability satellite.
//
// The classifier (bb_net_health_eval) is a pure, ESP-IDF-free function that
// takes a snapshot of WiFi RSSI and MQTT state and produces a health bucket
// with hysteresis.  It is host-testable with 100% branch coverage.
//
// The ESP-IDF glue (bb_net_health_register_health, bb_net_health_attach_sse)
// lives in platform/espidf/bb_net_health/bb_net_health_espidf.c and wires the pure
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
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// RSSI bucket thresholds (compile-time overridable).
//
// On ESP-IDF, Kconfig generates CONFIG_BB_NET_HEALTH_* symbols (different
// names from the public BB_NET_HEALTH_* knobs).  Bridge them here so that
// menuconfig changes actually take effect.  On the host build there is no
// sdkconfig, so we fall straight through to the numeric fallbacks.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_RSSI_GOOD
#    define BB_NET_HEALTH_RSSI_GOOD CONFIG_BB_NET_HEALTH_RSSI_GOOD
#  endif
#endif
#ifndef BB_NET_HEALTH_RSSI_GOOD
#define BB_NET_HEALTH_RSSI_GOOD     (-67)  // rssi >= -67 → GOOD
#endif

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_RSSI_MARGINAL_LO
#    define BB_NET_HEALTH_RSSI_MARGINAL_LO CONFIG_BB_NET_HEALTH_RSSI_MARGINAL_LO
#  endif
#endif
#ifndef BB_NET_HEALTH_RSSI_MARGINAL_LO
#define BB_NET_HEALTH_RSSI_MARGINAL_LO  (-75)  // -75 <= rssi <= -68 → MARGINAL
#endif

// Hysteresis sample counts
#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_HYST_DOWN
#    define BB_NET_HEALTH_HYST_DOWN CONFIG_BB_NET_HEALTH_HYST_DOWN
#  endif
#endif
#ifndef BB_NET_HEALTH_HYST_DOWN
#define BB_NET_HEALTH_HYST_DOWN  3  // consecutive worse-bucket samples before downgrade
#endif

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_HYST_UP
#    define BB_NET_HEALTH_HYST_UP CONFIG_BB_NET_HEALTH_HYST_UP
#  endif
#endif
#ifndef BB_NET_HEALTH_HYST_UP
#define BB_NET_HEALTH_HYST_UP    3  // consecutive better-bucket samples before upgrade
#endif

// ---------------------------------------------------------------------------
// Heap state thresholds (compile-time overridable via Kconfig).
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_HEAP_LOW_BYTES
#    define BB_NET_HEALTH_HEAP_LOW_BYTES CONFIG_BB_NET_HEALTH_HEAP_LOW_BYTES
#  endif
#  ifdef CONFIG_BB_NET_HEALTH_HEAP_CRITICAL_BYTES
#    define BB_NET_HEALTH_HEAP_CRITICAL_BYTES CONFIG_BB_NET_HEALTH_HEAP_CRITICAL_BYTES
#  endif
#  ifdef CONFIG_BB_NET_HEALTH_HEAP_TRACE
#    define BB_NET_HEALTH_HEAP_TRACE CONFIG_BB_NET_HEALTH_HEAP_TRACE
#  endif
#endif
#ifndef BB_NET_HEALTH_HEAP_LOW_BYTES
#define BB_NET_HEALTH_HEAP_LOW_BYTES      40000  // free heap bytes below which → LOW
#endif
#ifndef BB_NET_HEALTH_HEAP_CRITICAL_BYTES
#define BB_NET_HEALTH_HEAP_CRITICAL_BYTES 20000  // free heap bytes below which → CRITICAL
#endif
#ifndef BB_NET_HEALTH_HEAP_TRACE
#define BB_NET_HEALTH_HEAP_TRACE 0
#endif

// ---------------------------------------------------------------------------
// SSE ring sizing (B1-472)
//
// The retained "net.health" SSE ring is attached with this explicit
// max_entry via bb_event_routes_attach_ex2 (see bb_net_health_attach_sse in
// platform/espidf/bb_net_health/bb_net_health_espidf.c), above the
// bb_event_routes global default (CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY,
// 256) — the serialized snapshot (nested mqtt/http objects) measures ~352 B
// in the host test's synthetic worst-case (a real HW snapshot with narrower
// field values measured ~341 B; the difference is digit-width in a few
// integer fields, not a real discrepancy). Same shape as the
// update.available / info.build precedent (#616, B1-434/435/439). Shared
// here (not local to the espidf glue) so the host test's ring-size
// assertions reference the same symbol the production call site uses.
// ---------------------------------------------------------------------------
#ifndef BB_NET_HEALTH_SSE_MAX_ENTRY
#define BB_NET_HEALTH_SSE_MAX_ENTRY 512
#endif

// ---------------------------------------------------------------------------
// Diagnostic-state log heartbeat (observe-only telemetry-over-logs, KB#556).
//
// bb_net_health_should_log / bb_net_health_format_log back a structured
// bb_log_i() line emitted from the ESP-IDF evaluator (platform/espidf/
// bb_net_health/bb_net_health_espidf.c) each cycle where net_mode changes
// (immediate) or the periodic interval elapses (throttled heartbeat). The
// line rides the existing "log" bb_event topic (serial + future UDP log
// sink) so a no-route/zombie board that cannot serve HTTP/MQTT still
// reports its full diagnostic state. OBSERVE-ONLY — neither helper triggers
// any recovery action.
//
// The heartbeat is always compiled in (a few hundred bytes flash, zero
// heap) — there is no compile-time on/off gate. It is controlled purely at
// runtime via the dedicated "net_state" log tag's level (emitted from the
// ESP-IDF evaluator with bb_log_i, distinct from bb_net_health's own TAG so
// an operator can raise/lower just the heartbeat via
// esp_log_level_set("net_state", ...) without touching other bb_net_health
// logs). No reflash is required to toggle it.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_LOG_INTERVAL_S
#    define BB_NET_HEALTH_LOG_INTERVAL_S CONFIG_BB_NET_HEALTH_LOG_INTERVAL_S
#  endif
#endif
#ifndef BB_NET_HEALTH_LOG_INTERVAL_S
#define BB_NET_HEALTH_LOG_INTERVAL_S 60
#endif

// ---------------------------------------------------------------------------
// Heap state
// ---------------------------------------------------------------------------

/**
 * Coarse heap health bucket.  Zero-init is BB_HEAP_STATE_OK so host stubs and
 * uninitialised-state callers always get a sane default.
 */
typedef enum {
    BB_HEAP_STATE_OK       = 0,
    BB_HEAP_STATE_LOW      = 1,
    BB_HEAP_STATE_CRITICAL = 2,
} bb_heap_state_t;

/**
 * Pure heap classifier: maps total free heap bytes to a bb_heap_state_t bucket
 * against the BB_NET_HEALTH_HEAP_LOW_BYTES / BB_NET_HEALTH_HEAP_CRITICAL_BYTES
 * thresholds.  No side-effects; host-testable.
 */
bb_heap_state_t bb_net_health_classify_heap(size_t free_bytes);

/**
 * Return the latest heap state computed by the evaluator.
 * Thread-safe: reads a module-static set by the evaluator.
 * Returns BB_HEAP_STATE_OK on host (evaluator never runs).
 */
bb_heap_state_t bb_net_health_heap_state(void);

/**
 * Return a static string for a bb_heap_state_t value.
 * "ok", "low", or "critical".  Never returns NULL.
 */
const char *bb_heap_state_str(bb_heap_state_t state);

// ---------------------------------------------------------------------------
// WiFi discrimination mode — pure classifier over (associated, has_ip).
// ---------------------------------------------------------------------------

/**
 * Coarse WiFi connectivity discriminator, distinguishing "no IP while
 * associated" (zombie/DHCP failure) from "not associated at all" (out of
 * range, wrong creds, AP down). OBSERVE-ONLY — no recovery action is wired
 * to this classification; it exists purely for /api/diag/net and net.health
 * observability.
 */
typedef enum {
    BB_NET_MODE_OK             = 0, // associated && has_ip
    BB_NET_MODE_NO_IP          = 1, // associated && !has_ip
    BB_NET_MODE_NOT_ASSOCIATED = 2, // !associated
} bb_net_mode_t;

/**
 * Pure classifier: derives a bb_net_mode_t from the current association and
 * IP-acquisition state. No side-effects; host-testable. Inputs are sourced
 * from bb_wifi_is_associated() / bb_wifi_has_ip() by the ESP-IDF evaluator.
 */
bb_net_mode_t bb_net_health_classify_mode(bool associated, bool has_ip);

/**
 * Return a static string for a bb_net_mode_t value.
 * "ok", "no_ip", or "not_associated". Never returns NULL.
 */
const char *bb_net_mode_str(bb_net_mode_t mode);

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
    uint32_t disc_age_s;            // seconds since last WiFi disconnect (0 = connected)
    uint32_t mqtt_disc_age_s;       // seconds since last MQTT disconnect (0 = never / connected)
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
// Live snapshot accessor (ESP-IDF) — current net-health bucket without
// re-running eval. For telemetry (bb_pub_health) / diagnostics consumers.
// ---------------------------------------------------------------------------

typedef struct {
    bb_net_state_t state;                // classified bucket (GOOD/MARGINAL/POOR)
    bool           early_warning;        // pre-failure warning latch
    bool           throttled;            // adaptive backoff active
    int            rssi;
    bool           mqtt_connected;       // true when MQTT broker is reachable
    uint32_t       mqtt_reconnect_count;
    uint32_t       last_disconnect_reason; // WiFi disconnect reason (wi->disc_reason)
    uint32_t       disc_age_s;           // seconds since last WiFi disconnect (0 = connected)
    uint32_t       mqtt_disc_age_s;      // seconds since last MQTT disconnect (from evaluator)
    uint32_t       mqtt_disc_reason;     // classified MQTT disconnect reason (bb_mqtt_disc_t)
    uint32_t       mqtt_tls_fail;        // TLS handshake failure class (bb_tls_fail_t)
    bool          http_connected;       // true when HTTP sink session is open
    uint32_t      http_consec_failures; // consecutive HTTP transport failures
    uint32_t      http_tls_fail;        // TLS handshake failure class (bb_tls_fail_t)
    int           http_last_status;     // last HTTP status code (0 if none)
    uint32_t lost_ip_recoveries; // times lost-IP recovery was triggered (bb_wifi_get_lost_ip_count)
    uint32_t lost_ip_age_s;      // seconds since last lost-IP event (0 if never)
    uint32_t egress_dead_recoveries; // times egress-dead recovery was triggered (bb_wifi_get_egress_dead_count)
    uint32_t no_ip_recoveries;   // times no-IP watchdog recovery was triggered (bb_wifi_get_no_ip_count);
                                 // captured in the same evaluator snapshot as lost_ip/egress_dead_recoveries
                                 // (B1-486 finding #4) so GET /api/diag/net's recovery_count sums
                                 // point-in-time-consistent operands. Not currently serialized by
                                 // bb_net_health_emit (net.health SSE topic keeps its existing schema).
    uint32_t roam_count;   // times STA roamed to a different BSSID (bb_wifi_get_roam_count);
                            // OBSERVE-ONLY (B1-497) — no recovery action is associated with this
                            // counter. Captured in the same evaluator snapshot as the other
                            // recovery counters. Not serialized by bb_net_health_emit (net.health
                            // SSE topic keeps its existing schema) — same precedent as
                            // no_ip_recoveries; exposed via GET /api/diag/net instead.
    uint32_t roam_age_s;   // seconds since the last roam event (bb_wifi_get_roam_age_s); 0 if never
    uint32_t last_session_s; // duration of the most recently ENDED connected session, in
                              // seconds (bb_wifi_get_last_session_s); 0 if no session has
                              // ended yet since boot. OBSERVE-ONLY — captured in the same
                              // evaluator snapshot as the other recovery/discriminator
                              // counters. Serialized by bb_net_health_emit (GET /api/diag/net
                              // + net.health SSE) so drop cadence is visible without parsing
                              // logs.
    bb_net_mode_t net_mode; // WiFi discrimination mode (bb_net_health_classify_mode); OBSERVE-ONLY
    bool     associated;    // true iff STA is L2-associated (bb_wifi_is_associated)
    bool     has_ip;        // true iff STA has an IP (bb_wifi_has_ip)
    // --- Log-heartbeat-only fields (KB#556) — sourced from data the
    // evaluator already fetches each cycle (bb_wifi_info_t / bb_wifi
    // counters); NOT serialized by bb_net_health_emit (net.health SSE
    // topic keeps its existing schema), consumed only by
    // bb_net_health_format_log. ---
    char     ip[16];             // dotted-quad IPv4 (bb_wifi_info_t.ip), "0.0.0.0" if no IP.
    int      retry_count;        // STA retry attempts since last connect (bb_wifi_info_t.retry_count).
    uint32_t restart_sta_count;  // bb_wifi_get_restart_sta_count() — cumulative recovery restarts.
    uint32_t uptime_s;           // bb_clock_now_ms64()/1000 at evaluator snapshot time.
} bb_net_health_status_t;

// Copy the live net-health snapshot (populated by the ESP-IDF evaluator) under
// the cache lock. Returns BB_OK, or BB_ERR_INVALID_STATE before the evaluator
// has initialized (then *out is left untouched). *out fields are otherwise the
// last evaluated values (zero-init state=GOOD before the first eval).
bb_err_t bb_net_health_get_status(bb_net_health_status_t *out);

// ---------------------------------------------------------------------------
// Diagnostic-state log heartbeat helpers — pure, host-testable (KB#556).
// ---------------------------------------------------------------------------

/**
 * Decide whether the evaluator should emit a heartbeat log line this cycle.
 *
 * Returns true when EITHER:
 *  - mode != last_mode (a net_mode transition — logged immediately, ignoring
 *    interval_s), OR
 *  - now_us - last_log_us >= interval_s (periodic heartbeat, so a sustained
 *    zombie state keeps announcing itself).
 *
 * Pure — no side effects, no I/O. Caller owns and updates last_log_us /
 * last_mode across calls.
 */
bool bb_net_health_should_log(int64_t now_us, int64_t last_log_us,
                               bb_net_mode_t mode, bb_net_mode_t last_mode,
                               uint32_t interval_s);

/**
 * Format a compact key=val diagnostic-state line from a net-health snapshot
 * into buf. Fields are ordered critical-first (nm, ip, ip_ok, assoc, rssi,
 * sess, dr) so the line degrades gracefully under truncation against the
 * shared log pipeline caps (LOG_STREAM_LINE_MAX=192; the "log" bb_event
 * forwarder msgbuf=168, platform/espidf/bb_log/bb_log_event.c). Worst case
 * (max-length "255.255.255.255" ip, max uint32 counters) is ~212 bytes —
 * still exceeds both caps, but only the trailing counters (roam= onward)
 * are at risk: the critical-first prefix (nm/ip/ip_ok/assoc/rssi/sess/dr)
 * is ~92 bytes worst case, comfortably under the 168-byte forwarder cap, so
 * a truncated line always retains net_mode + ip — the fields needed to
 * diagnose a zombie board. The trailing counters are also available on
 * GET /api/diag/net, so losing them to truncation is acceptable. Always
 * null-terminates when cap > 0; truncates safely (never overflows buf)
 * when the formatted line would exceed cap.
 *
 * Fields (in emitted order): net_mode, ip, has_ip, associated, rssi,
 * session_s (last_session_s), disc_reason (code only — the per-drop log
 * event carries the human-readable name via bb_wifi_disc_reason_str),
 * roam_count, no_ip_recoveries, lost_ip_recoveries, egress_dead_recoveries,
 * retry_count, restart_sta_count, uptime_s. gateway_reachable is
 * intentionally omitted (not yet sampled by the evaluator).
 *
 * Pure — no ESP-IDF dependency. Returns the number of bytes that would have
 * been written (snprintf semantics), or 0 if s or buf is NULL or cap <= 0.
 */
int bb_net_health_format_log(const bb_net_health_status_t *s, char *buf, int cap);

// ---------------------------------------------------------------------------
// JSON serializer — single builder used by all three emitters (REST/SSE/pub).
// ---------------------------------------------------------------------------

/**
 * Emit net-health fields from snap into the JSON object obj.
 *
 * Top-level fields: rssi, state, early_warning, throttled,
 *   last_disconnect_reason (WiFi), disc_age_s, lost_ip_recoveries,
 *   lost_ip_age_s, egress_dead_recoveries, last_session_s.
 * Nested object "mqtt": connected, reconnect_count, disc_age_s,
 *   disc_reason, tls_fail.
 * Nested object "http": connected, consec_failures, tls_fail, last_status.
 *
 * Signature matches bb_cache_serialize_fn — pass directly to bb_cache_register.
 * Pure, host-testable — no ESP-IDF dependencies.
 */
void bb_net_health_emit(bb_json_t obj, const void *snap);

/**
 * Emit status-only (bools/enums) fields from snap into obj.
 *
 * Emits: state (string), early_warning (bool), throttled (bool),
 *   mqtt.connected (bool), http.connected (bool).
 *
 * No numeric fields. Used by /api/health "net" section (TA-505); numeric
 * counters are served by /api/diag/net via bb_net_health_emit.
 * Pure, host-testable — no ESP-IDF dependencies.
 */
void bb_net_health_emit_status(bb_json_t obj, const bb_net_health_status_t *snap);

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
 *   { "rssi": int, "state": string, "early_warning": bool, "throttled": bool,
 *     "last_disconnect_reason": uint (WiFi), "disc_age_s": uint,
 *     "mqtt": { "connected": bool, "reconnect_count": uint, "disc_age_s": uint,
 *               "disc_reason": uint, "tls_fail": uint } }
 *
 * Must be called before bb_http_server_start (before the health section
 * table is frozen).
 */
void bb_net_health_register_health(void);

/**
 * Attach the "net.health" retained SSE topic and publish an initial
 * snapshot immediately so SSE clients connecting before the first
 * evaluator tick receive current state.
 *
 * Must be called in the regular-tier init (after bb_event_routes_init) and
 * AFTER bb_net_health_start() has already created the evaluator's cache
 * mutex — returns BB_ERR_INVALID_STATE otherwise.
 */
bb_err_t bb_net_health_attach_sse(void);

/**
 * PRE_HTTP: starts the evaluator (state+timer). No HTTP side effects.
 * Autostarts when CONFIG_BB_NET_HEALTH_AUTOSTART=y.
 */
bb_err_t bb_net_health_start(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
