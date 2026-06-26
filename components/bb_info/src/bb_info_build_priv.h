#pragma once

// Private header for bb_info_build — not in include/ (not public API).
// Shared between platform/espidf/bb_info/bb_info.c and
// platform/host/bb_info/bb_info_host.c via relative include.

#include "bb_core.h"
#include "bb_json.h"
#include <stdint.h>

// Snapshot of all 13 build-time fields.
typedef struct {
    char     version[32];
    char     idf_version[32];
    char     build_date[16];
    char     build_time[16];
    char     project_name[64];
    char     chip_model[32];
    uint32_t chip_revision;
    uint8_t  cores;
    uint32_t cpu_freq_mhz;
    uint32_t flash_size;
    uint32_t app_size;
    char     board[64];
    char     app_sha256[17];  // up to 16 hex chars + NUL (default 9 + NUL)
} bb_info_build_snap_t;

// Pure serializer: emits all 13 keys from snap into obj.
// Signature matches bb_cache_serialize_fn.
void bb_info_build_emit(bb_json_t obj, const void *snap);

// Capture build-time data into out from bb_system_* and bb_board_* accessors.
// Compiles on both host and espidf — no platform-specific code in this layer.
bb_err_t bb_info_build_capture(bb_info_build_snap_t *out);
