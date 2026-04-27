#pragma once

#include "bb_nv.h"  // for bb_err_t (and bool via stdbool via nv_config)
#include <stdbool.h>
#include <stddef.h>

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
} bb_reset_reason_t;

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

#ifdef __cplusplus
}
#endif
