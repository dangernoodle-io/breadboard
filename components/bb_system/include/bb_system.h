#pragma once

#include "bb_core.h"  // for bb_err_t
#include "bb_reboot_reason.h"  // reboot-reason SSOT, re-exported here (B1-532 PR1)
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#define BB_ERROR_CHECK(x) ESP_ERROR_CHECK(x)
#else
#include <stdio.h>
#include <stdlib.h>
/// Checks bb_err_t; aborts with a diagnostic message on failure.
#define BB_ERROR_CHECK(x) do { \
    bb_err_t _err = (x); \
    if (_err != BB_OK) { \
        fprintf(stderr, "BB_ERROR_CHECK failed: %d at %s:%d\n", _err, __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_RESET_REASON_UNKNOWN = 0,
    BB_RESET_REASON_POWERON,
    BB_RESET_REASON_EXT,        // external pin (e.g. reset button)
    BB_RESET_REASON_SW,         // esp_restart / software
    BB_RESET_REASON_PANIC,      // exception / panic
    BB_RESET_REASON_INT_WDT,    // interrupt watchdog
    BB_RESET_REASON_TASK_WDT,   // task watchdog
    BB_RESET_REASON_WDT,        // other/generic watchdog
    BB_RESET_REASON_DEEPSLEEP,
    BB_RESET_REASON_BROWNOUT,
    BB_RESET_REASON_SDIO,
    BB_RESET_REASON_COUNT,      // sentinel — not a real reason, must stay last
} bb_reset_reason_t;

// Single source of truth for bb_reset_reason_t -> string mappings. X(reason, name)
// is invoked once per non-UNKNOWN reason. BB_RESET_REASON_UNKNOWN is intentionally
// excluded — every platform impl of bb_system_reset_reason_str() maps it (and any
// unrecognised value) to the "unknown" default case. Drives the three identical
// platform switches (espidf/host/arduino) so they collapse to one source of truth.
#define BB_RESET_REASON_LIST(X)              \
    X(BB_RESET_REASON_POWERON,   "power-on")  \
    X(BB_RESET_REASON_EXT,       "ext")       \
    X(BB_RESET_REASON_SW,        "software")  \
    X(BB_RESET_REASON_PANIC,     "panic")     \
    X(BB_RESET_REASON_INT_WDT,   "int_wdt")   \
    X(BB_RESET_REASON_TASK_WDT,  "task_wdt")  \
    X(BB_RESET_REASON_WDT,       "wdt")       \
    X(BB_RESET_REASON_DEEPSLEEP, "deep_sleep")\
    X(BB_RESET_REASON_BROWNOUT,  "brownout")  \
    X(BB_RESET_REASON_SDIO,      "sdio")

/// Reset reason for the current boot.
bb_reset_reason_t bb_system_get_reset_reason(void);

/// Short human-readable string for a reset reason. Never NULL.
const char *bb_system_reset_reason_str(bb_reset_reason_t r);

/// Returns true if this boot was caused by an abnormal reset
/// (panic, any WDT, brownout). Poweron/ext/SW/deepsleep are normal.
bool bb_system_is_abnormal_reset(void);

/// Log a one-line boot diagnostic via bb_log_i: reset reason + firmware version.
/// Call once early in app_main. Safe before NV init.
void bb_system_log_boot_info(void);

/// Returns the running firmware version string.
/// On ESP-IDF: thin wrapper over esp_app_get_description()->version.
/// On host/Arduino: compile-time BB_SYSTEM_VERSION_OVERRIDE or BB_FIRMWARE_VERSION, else "0.0.0".
/// Pointer is valid for the lifetime of the program; do not free.
const char *bb_system_get_version(void);

/// Returns the project name string.
/// On ESP-IDF: from esp_app_get_description()->project_name.
/// On host/Arduino: "host". Pointer is valid for program lifetime; do not free.
const char *bb_system_get_project_name(void);

/// Returns the firmware build date string (e.g. "Jan  1 2025").
/// On ESP-IDF: from esp_app_get_description()->date.
/// On host/Arduino: compiler __DATE__. Pointer is valid for program lifetime; do not free.
const char *bb_system_get_build_date(void);

/// Returns the firmware build time string (e.g. "12:34:56").
/// On ESP-IDF: from esp_app_get_description()->time.
/// On host/Arduino: compiler __TIME__. Pointer is valid for program lifetime; do not free.
const char *bb_system_get_build_time(void);

/// Returns the ESP-IDF version string used to build the firmware.
/// On ESP-IDF: from esp_app_get_description()->idf_ver.
/// On host/Arduino: "0.0.0-host". Pointer is valid for program lifetime; do not free.
const char *bb_system_get_idf_version(void);

/// Restarts the system.
/// On ESP-IDF: calls esp_restart() (does not return).
/// On host: prints a diagnostic to stderr and exits with code 0.
void bb_system_restart(void);

// ---------------------------------------------------------------------------
// Reboot-reason SSOT (B1-527 PR-A) — enum, wire-string mapping, and the pure
// record/history pack-unpack types live in bb_core/include/bb_reboot_reason.h
// (re-exported above, B1-532 PR1). bb_system owns only the action: persisting
// a record via bb_nv and restarting.
// ---------------------------------------------------------------------------

/// Persist a semantic reboot reason to NVS then restart. detail may be NULL;
/// non-NULL values are bounded to 48 chars (truncated at the first '|', if
/// any — see bb_reboot_record_encode).
/// On ESP-IDF: writes the record, then calls esp_restart() (does not return).
/// On host: prints a diagnostic to stderr and exits with code 0, mirroring
/// bb_system_restart().
void bb_system_restart_reason(bb_reset_source_t src, const char *detail);

/// Same as bb_system_restart_reason, but accepts a caller-supplied epoch
/// (e.g. from a client's clock) to record when the device has no NTP sync
/// of its own. See bb_reboot_pick_epoch for the selection rule: device NTP
/// time always wins when synced+valid; caller_epoch_s is the fallback;
/// otherwise 0. Pass caller_epoch_s=0 when the caller has no timestamp —
/// bb_system_restart_reason is exactly this call with caller_epoch_s=0.
void bb_system_restart_reason_at(bb_reset_source_t src, const char *detail, uint32_t caller_epoch_s);

// ---------------------------------------------------------------------------
// Boot-health counter (B1-753, part of the bb_nv dissolution epic B1-708) —
// bookkeeping for a device that never reaches a healthy WiFi connection
// after an OTA update. Callers increment on a WiFi-timeout-driven safeguard
// reboot and reset on a successful connect / OTA validation. Co-located
// with the reboot-reason record (same NVS namespace) since both are
// boot-health bookkeeping. Not incremented on every boot — only on the
// specific WiFi-timeout safeguard path — so a normal power-cycle never
// drifts the count.
// ---------------------------------------------------------------------------

/// Increments the boot-fail count by 1, saturating at UINT8_MAX. Persisted
/// to NVS on ESP-IDF; in-memory only (lost on process exit) on host.
bb_err_t bb_system_boot_count_increment(void);

/// Resets the boot-fail count to 0. Call on a successful WiFi connect or
/// OTA validation to clear the anti-brick window.
bb_err_t bb_system_boot_count_reset(void);

// ---------------------------------------------------------------------------
// Reboot budget (B1-863, WiFi FSM consolidation epic B1-790) — the shared
// cooldown + daily-cap rate limit for any consumer that wants to escalate to
// a reboot as a recovery action. Lifted out of bb_net_health's tier-3
// egress-recovery-ACT gate (B1-518 PR4) so a second consumer (the WiFi
// safeguard reboot, routed here in a later PR — B1-790 slice) can share the
// same budget machinery without re-hand-rolling it.
//
// PER-CAUSE state, SHARED params: each bb_reboot_cause_t gets its own
// persisted ring (so a flapping cause can never starve another cause's
// budget — see bb_system_reboot_budget_allows below), but every cause is
// rate-limited by the SAME two Kconfig knobs (BB_SYSTEM_REBOOT_BUDGET_
// MIN_INTERVAL_S / _DAILY_CAP) — there is exactly one pair of knobs, not one
// per cause.
//
// A consumer's OWN "has this fault been sustained long enough to consider
// rebooting" patience window (e.g. bb_net_health's
// CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_S) is NOT part of this budget and
// stays owned by that consumer — different causes arm on different fault
// conditions and gate different sustained-duration windows; this module
// only answers "is the shared cooldown/cap available right now for this
// cause".
// ---------------------------------------------------------------------------

/// Reboot causes sharing the bb_system reboot budget. Each cause is tracked
/// against its own persisted ring — exhausting one cause's budget never
/// blocks another's.
typedef enum {
    BB_REBOOT_CAUSE_WIFI_SAFEGUARD = 0, // wifi_reconn's persistent-fail safeguard reboot (not yet routed here — B1-790 slice)
    BB_REBOOT_CAUSE_EGRESS_TIER3,       // bb_net_health's sustained-gw-unreachable tier-3 reboot
    BB_REBOOT_CAUSE_COUNT,              // sentinel — not a real cause, must stay last
} bb_reboot_cause_t;

// Kconfig bridge (mirrors bb_net_health.h's own ESP_PLATFORM/CONFIG_ pattern
// — never a bare #ifndef alongside the CONFIG_ symbol, see kconfig-bridge-
// shadow).
#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_SYSTEM_REBOOT_BUDGET_MIN_INTERVAL_S
#    define BB_SYSTEM_REBOOT_BUDGET_MIN_INTERVAL_S CONFIG_BB_SYSTEM_REBOOT_BUDGET_MIN_INTERVAL_S
#  endif
#endif
#ifndef BB_SYSTEM_REBOOT_BUDGET_MIN_INTERVAL_S
#define BB_SYSTEM_REBOOT_BUDGET_MIN_INTERVAL_S 1800  // seconds between rate-limited reboots, per cause
#endif

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP
#    define BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP CONFIG_BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP
#  endif
#endif
#ifndef BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP
#define BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP 4  // max rate-limited reboots per rolling 24h, per cause
#endif

// Maximum ring capacity for reboot-timestamp persistence — must be >= the
// Kconfig BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP range max (10).
#define BB_SYSTEM_REBOOT_BUDGET_CAP_MAX 10

/**
 * Rolling reboot history for one cause, persisted across reboots.
 * Zero-init is valid (no reboots recorded yet).
 */
typedef struct {
    uint32_t last_reboot_s;                                  // epoch-s of most recent reboot for this cause, 0 = never
    uint32_t reboot_s_ring[BB_SYSTEM_REBOOT_BUDGET_CAP_MAX];  // ring of past reboot timestamps (epoch-s)
    uint8_t  ring_head;                                       // next write index
    uint8_t  ring_count;                                      // valid entries in the ring (saturates at CAP_MAX)
} bb_system_reboot_budget_state_t;

// Maximum encoded length (including NUL) of bb_system_reboot_budget_state_encode's
// output: "last_reboot_s|ring_head|ring_count|ts0,ts1,...,ts9" with every
// field at its worst-case (max uint32 / uint8) digit width.
#define BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX 192

/// Elapsed seconds between two epoch timestamps, clamped to 0 when now_s is
/// before since_s (clock skew — e.g. an uptime-derived clock reset by a
/// reboot) rather than wrapping via unsigned underflow. Shared by the
/// reboot-budget pure core below and by any consumer computing its own
/// epoch-based patience window (e.g. bb_net_health's tier-3 sustained-
/// unhealthy check) — never re-hand-roll this subtraction.
uint32_t bb_system_elapsed_epoch_s(uint32_t now_s, uint32_t since_s);

/**
 * Pure decision core: should a reboot be allowed right now, given the
 * shared cooldown/cap and one cause's persisted state? True iff BOTH:
 *  - st->last_reboot_s == 0 OR now_s - st->last_reboot_s >= min_interval_s
 *    (cooldown satisfied), AND
 *  - fewer than daily_cap entries in st->reboot_s_ring fall within the
 *    trailing 86400 s of now_s (daily cap not exhausted).
 *
 * No clock, no storage, no side effects — host-testable with explicit
 * now_s/params. Clock-skew safety: every elapsed-time computation goes
 * through bb_system_elapsed_epoch_s, so a skewed clock can only make this
 * function MORE conservative (fewer reboots), never trigger a spurious one.
 * The daily-cap ring count similarly excludes any ring entry whose
 * timestamp appears to be after now_s.
 */
bool bb_system_reboot_budget_should_allow(uint32_t                                now_s,
                                           uint32_t                                min_interval_s,
                                           uint32_t                                daily_cap,
                                           const bb_system_reboot_budget_state_t *st);

/**
 * Record a reboot into st: appends now_s to the ring (overwriting the
 * oldest entry once full), advances ring_head, bumps ring_count (saturating
 * at BB_SYSTEM_REBOOT_BUDGET_CAP_MAX), and sets last_reboot_s. No side
 * effects beyond mutating *st; host-testable.
 */
void bb_system_reboot_budget_state_record(bb_system_reboot_budget_state_t *st, uint32_t now_s);

/**
 * Encode a cause's reboot-budget state as a single delimited string:
 * "<last_reboot_s>|<ring_head>|<ring_count>|<ts0>,<ts1>,...,<tsN-1>" where N
 * is always BB_SYSTEM_REBOOT_BUDGET_CAP_MAX. Returns true and NUL-terminates
 * buf on success; false (buf left untouched beyond buf[0] on truncation
 * risk) if st or buf is NULL, buf_len is too small for the worst case, or
 * snprintf would have truncated. No side effects; host-testable.
 */
bool bb_system_reboot_budget_state_encode(const bb_system_reboot_budget_state_t *st,
                                           char *buf, size_t buf_len);

/**
 * Decode a string produced by bb_system_reboot_budget_state_encode back into
 * *out. Returns true on a well-formed round-trip; false (and *out left
 * untouched) on a NULL argument or malformed input — the caller's existing
 * zero-init state is the safe fallback for "never persisted" / corrupt
 * cases. No side effects; host-testable.
 */
bool bb_system_reboot_budget_state_decode(const char *str, bb_system_reboot_budget_state_t *out);

/**
 * Storage-backed orchestration for `cause`, given an EXPLICIT synced/now_s
 * pair (portable — compiled on host AND ESP-IDF, no ESP_PLATFORM gate
 * beyond the bb_storage backend-name pick: "nvs" on device, "ram" on
 * host). Each cause's state is loaded from storage ONCE (lazily, on first
 * synced access) and cached in RAM thereafter — an evaluator that re-checks
 * this every tick while sustained-unhealthy costs one storage read total,
 * not one per tick. Safe without a lock because each cause has exactly one
 * writer task; see the cache comment in bb_system_reboot_budget.c before
 * adding a second writer for the same cause. A missing key / backend
 * error / corrupt value on that one load all decode-fail safely to a
 * zero-init "no reboots recorded yet" state, then delegates to
 * bb_system_reboot_budget_should_allow.
 *
 * When synced is false, returns true unconditionally and performs NO
 * storage I/O (not even the lazy cache load) — the safe direction (a
 * spurious extra reboot vs. silently defeating the rate limit with bogus
 * epoch-0 arithmetic), mirrors bb_net_health's existing tier-3 "unsynced =
 * deferred" precedent. This explicit-args form is what makes the synced
 * branch host-testable — bb_system_reboot_budget_allows below hardcodes
 * synced=false on host (no NTP), so its own synced branch is otherwise
 * unreachable there.
 */
bool bb_system_reboot_budget_allows_at(bb_reboot_cause_t cause, bool synced, uint32_t now_s);

/**
 * Record that a reboot for `cause` is about to happen, given an explicit
 * synced/now_s pair: updates the cached state and writes it through to
 * storage (same cache as bb_system_reboot_budget_allows_at). Call
 * immediately before the actual restart (bb_system_restart_reason /
 * bb_system_restart) so the record survives the reboot. synced=false is a
 * no-op (symmetrical with bb_system_reboot_budget_allows_at) — no storage
 * I/O, not even a cache load.
 */
void bb_system_reboot_budget_record_at(bb_reboot_cause_t cause, bool synced, uint32_t now_s);

/**
 * Should a reboot for `cause` be allowed right now? Resolves the real wall
 * clock (bb_ntp_is_synced() + time()) per-platform and delegates to
 * bb_system_reboot_budget_allows_at — implemented separately per platform
 * (platform/espidf/bb_system/bb_system.c resolves the real clock;
 * platform/host/bb_system/bb_system_host.c is a straight-line
 * `_at(cause, false, 0U)`, since host has no NTP and bb_ntp_is_synced()
 * is hardcoded false there with no seam to force it true).
 */
bool bb_system_reboot_budget_allows(bb_reboot_cause_t cause);

/// Same per-platform split as bb_system_reboot_budget_allows, for
/// bb_system_reboot_budget_record_at.
void bb_system_reboot_budget_record(bb_reboot_cause_t cause);

/// Pure parse of POST /api/reboot's optional JSON body: {"ts": <epoch_s>,
/// "detail": "<string, up to 48 chars>"} — both fields optional. body may be
/// NULL/empty/non-JSON/oversized; on any parse failure out_ts=0 and
/// out_detail falls back per the precedence below. No platform deps beyond
/// bb_json (host-testable; compiled on host/ESP-IDF/Arduino).
///
/// ts: parsed from body["ts"] and clamped to (0, UINT32_MAX] before casting
/// (negative, zero, NaN/Inf, or >UINT32_MAX all yield out_ts=0).
///
/// detail precedence: body["detail"] (non-empty) > ua_or_null (non-NULL,
/// non-empty) > "". ua_or_null is the already-resolved caller identity
/// (e.g. a request's User-Agent header) — this function does not read any
/// header itself, keeping it request-independent and host-testable.
///
/// out_ts and out_detail must be non-NULL; out_detail is bounded to
/// out_detail_len (NUL-terminated, truncated if longer).
void bb_system_reboot_parse_body(const char *body, int body_len, const char *ua_or_null,
                                 uint32_t *out_ts, char *out_detail, size_t out_detail_len);

/// Reads the SoC internal die-temperature sensor.
/// Returns BB_OK and writes *out (degrees Celsius) on silicon that has the
/// modern temperature_sensor peripheral (esp32s2/s3/c3/c6/h2/...).
/// Returns BB_ERR_UNSUPPORTED on parts without it — notably the classic
/// ESP32, whose legacy sensor is uncalibrated and intentionally not surfaced —
/// and on host/Arduino backends. *out is untouched on error.
bb_err_t bb_system_read_temp_celsius(float *out);

/// Pure formatter for the CONFIG_BB_SYSTEM_BOOT_BANNER one-time boot banner line.
/// Any NULL input string is rendered as "?" rather than crashing — every
/// bb_system_get_* accessor above already promises a non-NULL, program-
/// lifetime pointer, but this keeps the formatter itself defensive and
/// independently host-testable. `out` is always NUL-terminated within
/// out_len (even when truncated). Returns the vsnprintf-style would-be
/// length (may be >= out_len to signal truncation — callers may still log
/// the buffer as-is), or -1 if out is NULL or out_len is 0 (no write).
int bb_system_boot_banner_format(char *out, size_t out_len,
                                  const char *project, const char *version,
                                  const char *build_date, const char *build_time,
                                  const char *idf_version);

/// Writes the first N hex characters of the app ELF SHA256 into out.
/// N is controlled by CONFIG_APP_RETRIEVE_LEN_ELF_SHA (default 9 on ESP-IDF).
/// On host, writes "deadbeef0" (9 chars, fixed test value).
/// out must be at least out_size bytes; returns BB_ERR_NO_SPACE if
/// out_size is too small to hold N chars + NUL terminator.
bb_err_t bb_system_get_app_sha256(char *out, size_t out_size);

/// Registry hook — emits the CONFIG_BB_SYSTEM_BOOT_BANNER one-time boot
/// banner line via bb_log_i.
// bbtool:init tier=early fn=bb_system_boot_banner_init
bb_err_t bb_system_boot_banner_init(void);

#ifdef ESP_PLATFORM
#include "bb_http_server.h"

/// Reserve route-table slots for bb_system before the HTTP server starts.
// bbtool:init tier=pre_http fn=bb_system_routes_reserve
bb_err_t bb_system_routes_reserve(void);

/// Registry hook — registers POST /api/reboot.
// bbtool:init tier=regular fn=bb_system_routes_init server=true
bb_err_t bb_system_routes_init(bb_http_handle_t server);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
