#pragma once

// Reboot-reason SSOT (B1-527 PR-A/PR-B, re-homed to bb_core in B1-532 PR1).
// Pure string/struct pack-unpack — no ESP-IDF, no bb_json. Compiled on host,
// ESP-IDF, and Arduino. bb_system re-exports these declarations via
// bb_system.h for existing includers; new/updated call sites may include
// this header directly.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Semantic reason a reboot was requested. Closed enum — any new site that
/// intentionally reboots the device adds a value here rather than calling
/// bb_system_restart() / esp_restart() directly.
typedef enum {
    BB_RESET_SRC_UNKNOWN = 0,
    BB_RESET_SRC_API_REBOOT,
    BB_RESET_SRC_FACTORY_RESET,
    BB_RESET_SRC_WIFI_SAFEGUARD,
    BB_RESET_SRC_WIFI_COLD_TIMEOUT,
    BB_RESET_SRC_WIFI_PENDING_REVERT,
    BB_RESET_SRC_WIFI_RECONFIGURE,
    BB_RESET_SRC_EGRESS_TIER3,
    BB_RESET_SRC_OTA_PULL_APPLIED,
    BB_RESET_SRC_OTA_PUSH_APPLIED,
    BB_RESET_SRC_OTA_BOOT_APPLY,
    BB_RESET_SRC_OTA_BOOT_DONE,
    BB_RESET_SRC_OTA_BOOT_ABORT,
    BB_RESET_SRC__COUNT,  // sentinel — not a real reason, must stay last
} bb_reset_source_t;

// Single source of truth for bb_reset_source_t -> wire-string mappings. X(src, name)
// is invoked once per non-UNKNOWN source. BB_RESET_SRC_UNKNOWN is intentionally
// excluded — bb_reset_source_str() maps it (and any unrecognised value) to the
// "unknown" default case. Wire tokens are stable JSON values consumed by clients.
#define BB_RESET_SRC_LIST(X)                                            \
    X(BB_RESET_SRC_API_REBOOT,          "api_reboot")                   \
    X(BB_RESET_SRC_FACTORY_RESET,       "factory_reset")                \
    X(BB_RESET_SRC_WIFI_SAFEGUARD,      "wifi_safeguard")                \
    X(BB_RESET_SRC_WIFI_COLD_TIMEOUT,   "wifi_cold_timeout")            \
    X(BB_RESET_SRC_WIFI_PENDING_REVERT, "wifi_pending_revert")          \
    X(BB_RESET_SRC_WIFI_RECONFIGURE,    "wifi_reconfigure")             \
    X(BB_RESET_SRC_EGRESS_TIER3,        "egress_tier3")                 \
    X(BB_RESET_SRC_OTA_PULL_APPLIED,    "ota_pull_applied")             \
    X(BB_RESET_SRC_OTA_PUSH_APPLIED,    "ota_push_applied")             \
    X(BB_RESET_SRC_OTA_BOOT_APPLY,      "ota_boot_apply")               \
    X(BB_RESET_SRC_OTA_BOOT_DONE,       "ota_boot_done")                \
    X(BB_RESET_SRC_OTA_BOOT_ABORT,      "ota_boot_abort")

/// Wire string for a reset-source enum value. Never NULL. Out-of-range values
/// (including BB_RESET_SRC_UNKNOWN) map to "unknown". Pure — no platform deps.
const char *bb_reset_source_str(bb_reset_source_t src);

// ---------------------------------------------------------------------------
// Reboot record — pure pack/unpack (host-testable, compiled on all platforms)
// ---------------------------------------------------------------------------

/// Single last-reboot record persisted under BB_REBOOT_NVS_NS/BB_REBOOT_KEY_LAST.
/// Cleared on read (single record, not a history ring — see B1-527 PR-A).
typedef struct {
    uint8_t  src;           ///< bb_reset_source_t
    char     detail[49];    ///< NUL-terminated, <=48 chars, never contains '|'
    uint32_t epoch_s;       ///< wall-clock seconds at reboot; 0 if unknown/unsynced
    uint32_t uptime_s;      ///< prior-session uptime (seconds) at reboot
} bb_reboot_record_t;

/// Maximum encoded length (including NUL) of bb_reboot_record_encode's output.
/// "<src 0-255>|<epoch_s>|<uptime_s>|<detail up to 48 chars>" + NUL.
#define BB_REBOOT_RECORD_STR_MAX 96

/// Encode a reboot record into a single delimited string:
///   "<src>|<epoch_s>|<uptime_s>|<detail>"
/// detail is freeform and placed last; any '|' inside detail is truncated at
/// (the delimiter and everything after it is dropped) so the fixed-field
/// parse in bb_reboot_record_decode stays unambiguous. Returns false on NULL
/// args, a zero-length buffer, or if the encoded string would not fit in
/// buf_len (buf is left in an unspecified but NUL-safe state in that case).
bool bb_reboot_record_encode(const bb_reboot_record_t *r, char *buf, size_t buf_len);

/// Decode a string produced by bb_reboot_record_encode back into *out.
/// Returns false on NULL args or malformed input (including an out-of-range
/// src field) and leaves *out untouched — callers should zero-init their own
/// struct as the safe fallback before calling this.
bool bb_reboot_record_decode(const char *str, bb_reboot_record_t *out);

// Sanity floor for a recorded epoch: 2024-01-01T00:00:00Z. Values below this
// (device or caller-supplied) are treated as invalid/unset.
#define BB_REBOOT_EPOCH_FLOOR_S 1704067200U

/// Pick the epoch to record on a reboot. Pure — no platform deps.
/// Device NTP wins when ntp_synced && device_epoch_s >= floor_s; otherwise
/// caller_epoch_s is used when it is >= floor_s; otherwise 0. floor_s is a
/// sanity floor (e.g. BB_REBOOT_EPOCH_FLOOR_S) below which a value is
/// treated as invalid/unset.
uint32_t bb_reboot_pick_epoch(bool ntp_synced, uint32_t device_epoch_s,
                               uint32_t caller_epoch_s, uint32_t floor_s);

// ---------------------------------------------------------------------------
// Reboot history — rolling ring of the last N reboots (host-testable, pure
// pack/unpack, B1-527 PR-B). Distinct from bb_reboot_record_t (the single,
// clear-on-read "last reboot" record): the history ring accumulates across
// boots (NOT cleared-on-read) and captures every boot this firmware sees,
// including untagged/hardware resets (recorded as src=BB_RESET_SRC_UNKNOWN),
// not just app-requested reboots that went through bb_system_restart_reason.
// ---------------------------------------------------------------------------

/// Ring capacity: the last N reboots retained.
#define BB_REBOOT_HISTORY_CAP 8

/// Single ring entry — minimal fields only (no detail string; that stays on
/// the single last-reboot record).
typedef struct {
    uint8_t  src;       ///< bb_reset_source_t
    uint32_t epoch_s;   ///< wall-clock seconds at reboot; 0 if unknown/unsynced
    uint32_t uptime_s;  ///< prior-session uptime (seconds) at reboot
} bb_reboot_hist_entry_t;

/// Rolling ring of the last BB_REBOOT_HISTORY_CAP reboots.
typedef struct {
    bb_reboot_hist_entry_t entries[BB_REBOOT_HISTORY_CAP];
    uint8_t head;   ///< index of the oldest entry
    uint8_t count;  ///< number of occupied entries, 0..BB_REBOOT_HISTORY_CAP
} bb_reboot_history_t;

/// Maximum encoded length (including NUL) of bb_reboot_history_encode's
/// output: "<head>|<count>|" header plus BB_REBOOT_HISTORY_CAP entries of
/// "<src>,<epoch_s>,<uptime_s>" separated by ';'.
#define BB_REBOOT_HISTORY_STR_MAX 256

/// Append a newest entry to the ring, evicting the oldest once at capacity.
/// No-op if h or e is NULL.
void bb_reboot_history_push(bb_reboot_history_t *h, const bb_reboot_hist_entry_t *e);

/// Encode a reboot history ring into a single delimited string:
///   "<head>|<count>|<src0>,<epoch0>,<uptime0>;...;<src7>,<epoch7>,<uptime7>"
/// All BB_REBOOT_HISTORY_CAP slots are always encoded regardless of count
/// (unoccupied slots are zero-valued). Returns false on NULL args, a
/// zero-length buffer, or if the encoded string would not fit in buf_len
/// (buf is left in an unspecified but NUL-safe state in that case).
bool bb_reboot_history_encode(const bb_reboot_history_t *h, char *buf, size_t buf_len);

/// Decode a string produced by bb_reboot_history_encode back into *out.
/// Returns false on NULL args or malformed input (including out-of-range
/// head/count/src fields) and leaves *out untouched — callers should
/// zero-init their own struct as the safe fallback before calling this.
bool bb_reboot_history_decode(const char *str, bb_reboot_history_t *out);

#ifdef __cplusplus
}
#endif
