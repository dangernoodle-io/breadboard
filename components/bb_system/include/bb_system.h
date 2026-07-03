#pragma once

#include "bb_core.h"  // for bb_err_t
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
// Reboot-reason SSOT (B1-527 PR-A)
// ---------------------------------------------------------------------------

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

/// Persist a semantic reboot reason to NVS then restart. detail may be NULL;
/// non-NULL values are bounded to 48 chars (truncated at the first '|', if
/// any — see bb_reboot_record_encode).
/// On ESP-IDF: writes the record, then calls esp_restart() (does not return).
/// On host: prints a diagnostic to stderr and exits with code 0, mirroring
/// bb_system_restart().
void bb_system_restart_reason(bb_reset_source_t src, const char *detail);

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

/// Reads the SoC internal die-temperature sensor.
/// Returns BB_OK and writes *out (degrees Celsius) on silicon that has the
/// modern temperature_sensor peripheral (esp32s2/s3/c3/c6/h2/...).
/// Returns BB_ERR_UNSUPPORTED on parts without it — notably the classic
/// ESP32, whose legacy sensor is uncalibrated and intentionally not surfaced —
/// and on host/Arduino backends. *out is untouched on error.
bb_err_t bb_system_read_temp_celsius(float *out);

/// Writes the first N hex characters of the app ELF SHA256 into out.
/// N is controlled by CONFIG_APP_RETRIEVE_LEN_ELF_SHA (default 9 on ESP-IDF).
/// On host, writes "deadbeef0" (9 chars, fixed test value).
/// out must be at least out_size bytes; returns BB_ERR_NO_SPACE if
/// out_size is too small to hold N chars + NUL terminator.
bb_err_t bb_system_get_app_sha256(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
