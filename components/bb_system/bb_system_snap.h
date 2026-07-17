#pragma once

// Private: format-agnostic system-snapshot descriptor, owned by bb_system
// (B1-767-family, additive-only -- not wired into any consumer yet).
//
// bb_system has no existing bundling snapshot struct (only discrete
// getters) -- bb_system_snap_t is a new, small POD assembled by
// bb_system_snap_fill() below from those existing accessors. Every accessor
// it draws from is already portable (a real value on ESP-IDF, a documented
// host/Arduino fallback everywhere else -- see bb_system.h's own doc
// comments), so bb_system_snap_fill() itself needs no ESP_PLATFORM gate.

#include "bb_serialize.h"

#include "bb_core.h"

#include <stdint.h>

// Fixed embedded-string capacities -- generous headroom over every observed
// value shape (see bb_system.h's per-getter doc comments for the host/
// ESP-IDF value shapes these bound):
//   version/project_name/idf_version: ESP-IDF's esp_app_desc_t caps each at
//     32 bytes (including NUL) -- mirrored here.
//   reset_reason: longest entry in BB_RESET_REASON_LIST is "deep_sleep" (10
//     chars); 16 leaves headroom.
//   build_date/build_time: compiler __DATE__/__TIME__ are fixed formats
//     ("Jan  1 2025" / "12:34:56", 11/8 chars); 16 leaves headroom.
#define BB_SYSTEM_SNAP_VERSION_MAX_LEN      32
#define BB_SYSTEM_SNAP_PROJECT_NAME_MAX_LEN 32
#define BB_SYSTEM_SNAP_IDF_VERSION_MAX_LEN  32
#define BB_SYSTEM_SNAP_RESET_REASON_MAX_LEN 16
#define BB_SYSTEM_SNAP_BUILD_DATE_MAX_LEN   16
#define BB_SYSTEM_SNAP_BUILD_TIME_MAX_LEN   16

// Root -- identity + boot-state fields for a system diagnostic snapshot.
// All fixed-size, heap-free.
typedef struct {
    char     version[BB_SYSTEM_SNAP_VERSION_MAX_LEN];
    char     project_name[BB_SYSTEM_SNAP_PROJECT_NAME_MAX_LEN];
    char     idf_version[BB_SYSTEM_SNAP_IDF_VERSION_MAX_LEN];
    char     reset_reason[BB_SYSTEM_SNAP_RESET_REASON_MAX_LEN];
    uint64_t boot_count;
    char     build_date[BB_SYSTEM_SNAP_BUILD_DATE_MAX_LEN];
    char     build_time[BB_SYSTEM_SNAP_BUILD_TIME_MAX_LEN];
} bb_system_snap_t;

extern const bb_serialize_desc_t bb_system_snap_desc;

// Populates `out` from bb_system's existing portable accessors
// (bb_system_get_version/_get_project_name/_get_idf_version/
// _reset_reason_str(_get_reset_reason())/_boot_count_get/_get_build_date/
// _get_build_time). Every accessor already has a defined host/Arduino
// fallback (see bb_system.h), so this fill is portable with no
// ESP_PLATFORM gate. Returns BB_ERR_INVALID_ARG if `out` is NULL; BB_OK
// otherwise.
bb_err_t bb_system_snap_fill(bb_system_snap_t *out);
