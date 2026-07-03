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
// Egress-recovery SSOT (B1-518) — Phase 1 pure classifier.
//
// OBSERVE-ONLY: this classifier drives logging/counters only; no recovery
// action is wired to it (that is Phase 3 of B1-518). It distinguishes three
// distinct failure shapes so a future recovery action can target the right
// layer instead of always restarting the WiFi stack:
//   - GW_UNREACHABLE: the WiFi link itself cannot reach the gateway — a
//     WiFi-layer problem.
//   - ENDPOINT_DOWN / ALL_DEAD: the gateway is reachable but one or all
//     egress clients (e.g. MQTT broker, mining pool) are failing — an
//     upstream/application-layer problem, NOT a WiFi fault.
// ---------------------------------------------------------------------------

/**
 * Egress health state, derived from wifi_mode plus a gateway reachability
 * probe and egress-client failure counts.
 */
typedef enum {
    BB_EGRESS_STATE_OK             = 0, // gw reachable, or not enough data / not applicable
    BB_EGRESS_STATE_ENDPOINT_DOWN  = 1, // gw reachable but >=1 (not all) egress clients failing
    BB_EGRESS_STATE_GW_UNREACHABLE = 2, // gw probe failed >= threshold consecutive times
    BB_EGRESS_STATE_ALL_DEAD       = 3, // gw reachable but ALL enabled egress clients failing
} bb_egress_state_t;

/**
 * Return a static string for a bb_egress_state_t value.
 * "ok", "endpoint_down", "gw_unreachable", or "all_dead". Never returns NULL.
 */
const char *bb_egress_state_str(bb_egress_state_t s);

/**
 * Pure classifier: derives a bb_egress_state_t from the current WiFi
 * discrimination mode, gateway-probe results, and egress-client failure
 * counts. No side-effects; host-testable.
 *
 * The gateway probe is the tiebreaker between a WiFi-layer fault and an
 * upstream/application-layer fault: when the gateway IS reachable but an
 * egress client (e.g. the mining pool) is down, that must classify as
 * ENDPOINT_DOWN — never GW_UNREACHABLE/ALL_DEAD — so a future recovery
 * action does not restart WiFi to fix a problem WiFi cannot fix.
 *
 * @param wifi_mode          Current bb_net_health_classify_mode() result.
 *                            Only BB_NET_MODE_OK is evaluated further — the
 *                            NOT_ASSOCIATED/NO_IP cases are owned by the
 *                            wifi FSM / no-IP watchdog, not this classifier.
 * @param gw_probed           True once at least one gateway probe attempt
 *                            has completed (false = no probe data yet).
 * @param gw_reachable        Result of the most recent gateway probe.
 * @param gw_fail_streak      Consecutive gateway-probe failures.
 * @param gw_fail_threshold   Consecutive-failure count required to declare
 *                            the gateway unreachable.
 * @param enabled_egress_count Number of egress clients currently enabled.
 * @param failing_egress_count Number of those clients currently failing.
 */
bb_egress_state_t bb_net_health_classify_egress(bb_net_mode_t wifi_mode,
                                                 bool          gw_probed,
                                                 bool          gw_reachable,
                                                 uint8_t       gw_fail_streak,
                                                 uint8_t       gw_fail_threshold,
                                                 int           enabled_egress_count,
                                                 int           failing_egress_count);

/**
 * Pure edge-check predicate for the evaluator's "would recover" log
 * (B1-518 PR3, OBSERVE-ONLY): true only on the transition INTO
 * BB_EGRESS_STATE_GW_UNREACHABLE — the only egress_state that would trigger
 * a WiFi restart under a future act gate (egress failing AND gateway
 * unreachable). Sustained GW_UNREACHABLE across ticks does not re-log
 * (prev == cur == GW_UNREACHABLE returns false); a departure and later
 * re-entry logs again. No side effects; host-testable.
 *
 * @param prev  egress_state observed on the previous evaluator cycle.
 * @param cur   egress_state observed on the current evaluator cycle.
 */
bool bb_net_health_would_recover_edge(bb_egress_state_t prev, bb_egress_state_t cur);

// ---------------------------------------------------------------------------
// Egress-recovery ACT gate (B1-518 PR4) — pure predicates, ALWAYS compiled
// (host-testable). The device-side call sites that invoke esp_restart() /
// bb_wifi_request_recovery() are gated behind
// #if CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE in
// platform/espidf/bb_net_health/bb_net_health_espidf.c — these predicates
// have no ESP-IDF dependency and no side effects.
//
// All time values are epoch-SECONDS (uint32_t) sourced from time(NULL) —
// NEVER a monotonic/uptime-derived clock. An uptime-derived "now_s" resets
// to 0 across esp_restart(), which would silently defeat the persisted
// daily-cap / min-interval rate limits below and reopen a reboot-loop risk;
// epoch time survives the restart because it is re-synced from NTP.
//
// The device-side call site (platform/espidf/bb_net_health/bb_net_health_espidf.c)
// gates the ENTIRE tier-3 (reboot) path on bb_ntp_is_synced(): when the
// clock has never been synchronized, tier-3 is LOG-ONLY — no esp_restart(),
// no NVS persist, no ring update. A board that never syncs NTP simply never
// tier-3-reboots, which is the safe direction (never a spurious reboot from
// bogus epoch-0 arithmetic). Tier-2 (bb_wifi_request_recovery) is UNGATED —
// it is edge-triggered off egress_state transitions and needs no epoch time.
//
// Callers must still guard against clock *skew* (e.g. an NTP step-back)
// themselves via the elapsed-time semantics documented on
// bb_net_health_should_reboot below; these functions never wrap on
// underflow.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_S
#    define BB_NET_HEALTH_EGRESS_ACT_REBOOT_S CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_S
#  endif
#endif
#ifndef BB_NET_HEALTH_EGRESS_ACT_REBOOT_S
#define BB_NET_HEALTH_EGRESS_ACT_REBOOT_S 480  // seconds of sustained GW_UNREACHABLE before tier-3 reboot
#endif

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_MIN_INTERVAL_S
#    define BB_NET_HEALTH_EGRESS_ACT_REBOOT_MIN_INTERVAL_S CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_MIN_INTERVAL_S
#  endif
#endif
#ifndef BB_NET_HEALTH_EGRESS_ACT_REBOOT_MIN_INTERVAL_S
#define BB_NET_HEALTH_EGRESS_ACT_REBOOT_MIN_INTERVAL_S 1800  // seconds
#endif

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_DAILY_CAP
#    define BB_NET_HEALTH_EGRESS_ACT_REBOOT_DAILY_CAP CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_DAILY_CAP
#  endif
#endif
#ifndef BB_NET_HEALTH_EGRESS_ACT_REBOOT_DAILY_CAP
#define BB_NET_HEALTH_EGRESS_ACT_REBOOT_DAILY_CAP 4
#endif

// Maximum ring capacity for reboot-timestamp persistence — must be >= the
// Kconfig BB_NET_HEALTH_EGRESS_ACT_REBOOT_DAILY_CAP range max (10).
#define BB_NET_HEALTH_REBOOT_CAP_MAX 10

/**
 * Rolling tier-3 reboot history, persisted across reboots (NVS namespace
 * bb_egress_act). Zero-init is valid (no reboots recorded yet).
 */
typedef struct {
    uint32_t last_reboot_s;                              // epoch-s of most recent tier-3 reboot, 0 = never
    uint32_t reboot_s_ring[BB_NET_HEALTH_REBOOT_CAP_MAX]; // ring of past reboot timestamps (epoch-s)
    uint8_t  ring_head;                                   // next write index
    uint8_t  ring_count;                                  // valid entries in the ring (saturates at CAP_MAX)
} bb_net_health_reboot_state_t;

/**
 * Tier-2 predicate: should the evaluator call bb_wifi_request_recovery()
 * this cycle? True iff act_enabled AND the egress_state transitioned INTO
 * BB_EGRESS_STATE_GW_UNREACHABLE this cycle (bb_net_health_would_recover_edge).
 * No side effects; host-testable.
 */
bool bb_net_health_should_request_recovery(bb_egress_state_t prev,
                                            bb_egress_state_t cur,
                                            bool               act_enabled);

/**
 * Tier-3 predicate: should the evaluator escalate to esp_restart() this
 * cycle? True iff ALL of:
 *  - unhealthy_since_s != 0 (currently in a sustained-unhealthy window), AND
 *  - now_s - unhealthy_since_s >= t_reboot_s (sustained long enough), AND
 *  - st->last_reboot_s == 0 OR now_s - st->last_reboot_s >= min_interval_s
 *    (rate-limit satisfied), AND
 *  - fewer than daily_cap entries in st->reboot_s_ring fall within the
 *    trailing 86400 s of now_s (daily cap not exhausted).
 *
 * Clock-skew safety: every elapsed-time computation clamps to 0 when
 * now_s is before the stored timestamp (e.g. an uptime-derived "now_s"
 * reset by a reboot) rather than wrapping via unsigned underflow — an
 * elapsed time of 0 never satisfies a ">= threshold" check, so a skewed
 * clock can only make this function MORE conservative (fewer reboots),
 * never trigger a spurious one. The daily-cap ring count similarly
 * excludes (does not count) any ring entry whose timestamp appears to be
 * after now_s.
 *
 * No side effects (does not update st); host-testable.
 */
bool bb_net_health_should_reboot(uint32_t                            unhealthy_since_s,
                                  uint32_t                            now_s,
                                  uint32_t                            t_reboot_s,
                                  uint32_t                            min_interval_s,
                                  uint32_t                            daily_cap,
                                  const bb_net_health_reboot_state_t *st);

/**
 * Record a tier-3 reboot into st: appends now_s to the ring (overwriting
 * the oldest entry once full), advances ring_head, bumps ring_count
 * (saturating at BB_NET_HEALTH_REBOOT_CAP_MAX), and sets last_reboot_s.
 * No side effects beyond mutating *st; host-testable.
 */
void bb_net_health_reboot_state_record(bb_net_health_reboot_state_t *st, uint32_t now_s);

// Maximum encoded length (including NUL) of bb_net_health_reboot_state_encode's
// output: "last_reboot_s|ring_head|ring_count|ts0,ts1,...,ts9" with every
// field at its worst-case (max uint32 / uint8) digit width. Sized generously
// so callers can size a fixed stack buffer without recomputing the math.
#define BB_NET_HEALTH_REBOOT_STATE_STR_MAX 192

/**
 * Encode the reboot-rate-limit state as a single delimited string:
 * "<last_reboot_s>|<ring_head>|<ring_count>|<ts0>,<ts1>,...,<tsN-1>" where N
 * is always BB_NET_HEALTH_REBOOT_CAP_MAX (every ring slot is encoded,
 * regardless of ring_count, so decode never needs to guess trailing zeros).
 * Collapses the reboot-state persistence to a single bb_nv_set_str key/commit
 * instead of one commit per scalar field.
 *
 * Returns true and NUL-terminates buf on success; false (buf left untouched
 * beyond buf[0] on truncation risk) if st or buf is NULL, buf_len is too
 * small for the worst case, or snprintf would have truncated. No side
 * effects; host-testable.
 */
bool bb_net_health_reboot_state_encode(const bb_net_health_reboot_state_t *st,
                                        char *buf, size_t buf_len);

/**
 * Decode a string produced by bb_net_health_reboot_state_encode back into
 * *out. Returns true on a well-formed round-trip; false (and *out left
 * untouched) on a NULL argument or malformed input (wrong field count, a
 * ring_head/ring_count out of range, or a non-numeric token) — the caller's
 * existing zero-init *st is the safe fallback for "never persisted" /
 * corrupt-NVS cases. No side effects; host-testable.
 */
bool bb_net_health_reboot_state_decode(const char *str,
                                        bb_net_health_reboot_state_t *out);

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
    // Gateway-probe status (B1-518 PR3, OBSERVE-ONLY): pulled each evaluator
    // cycle from bb_wifi_get_gateway_status(). gw_available is false when the
    // probe worker has never started (CONFIG_BB_WIFI_GW_PROBE_ENABLE=n, or
    // BB_ERR_INVALID_STATE) OR has started but not yet completed a probe
    // (last_gw_probe_ms == 0) — in both cases the remaining gw_* fields are
    // zeroed and must not be treated as real data. No recovery action is
    // wired to these fields; they exist purely for /api/diag/net and the
    // net_state log heartbeat.
    bool     gw_available;   // true iff at least one gateway probe has completed
    bool     gw_reachable;   // result of the most recent gateway ping
    uint8_t  gw_fail_streak; // consecutive probe failures (observe-owned, separate from live FSM)
    uint32_t gw_dead_count;  // cumulative times the observe-only classifier would have tripped recovery
    uint64_t last_gw_probe_ms; // bb_clock_now_ms64() at the last probe, 0 = never run
    // bb_transport_health cached counts (B1-518 PR2, OBSERVE-ONLY): pulled
    // each evaluator cycle from bb_transport_health_authoritative_counts().
    // tx_available is false when the call fails (e.g. host build without the
    // registry initialized) OR when no AUTHORITATIVE transport is currently
    // enabled — in both cases tx_enabled/tx_failing must not be treated as
    // real data. No recovery action is wired to these fields.
    bool     tx_available;   // true iff the counts call succeeded and enabled > 0
    int      tx_enabled;     // count of enabled AUTHORITATIVE transports
    int      tx_failing;     // count of those currently failing
    // Egress-recovery SSOT classification (B1-518 PR3, OBSERVE-ONLY): derived
    // each evaluator cycle from bb_net_health_classify_egress(net_mode,
    // gw_available, gw_reachable, gw_fail_streak, BB_WIFI_GW_PROBE_FAILS,
    // tx_enabled, tx_failing). No recovery action is wired to this field; it
    // exists purely for /api/diag/net and the net_state log heartbeat
    // ("egr=" token, emitted only when != BB_EGRESS_STATE_OK). Not currently
    // serialized by bb_net_health_emit (net.health SSE topic keeps its
    // existing schema) — same precedent as gw_available/tx_available.
    bb_egress_state_t egress_state;
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
 * retry_count, restart_sta_count, uptime_s, then — ONLY when
 * s->gw_available — gw (gw_reachable) and gwdead (gw_dead_count), and
 * then — ONLY when s->tx_available — txfail (tx_failing/tx_enabled from
 * bb_transport_health), and finally — ONLY when s->egress_state !=
 * BB_EGRESS_STATE_OK — egr (bb_egress_state_str(s->egress_state)), appended
 * in that order (gw, then txfail, then egr) so a truncated line drops the
 * LEAST critical field (egr) first, then txfail, then gw, before any of the
 * fields above. Each suffix is omitted entirely (no trailing space) when its
 * availability flag is false / condition unmet, so boards without
 * CONFIG_BB_WIFI_GW_PROBE_ENABLE, without any registered bb_transport_health
 * transport, or with a healthy (OK) egress_state see an unchanged heartbeat
 * line.
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
