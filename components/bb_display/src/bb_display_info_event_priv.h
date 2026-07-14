#pragma once
// Private shared header: bb_cache snapshot struct and serializer for the
// health.display retained event topic, owned by bb_display itself
// (B1-893 -- re-homed from the deleted bb_display_info satellite; this
// cache/SSE surface is independent of bb_info and stays live). Names kept
// as "bb_display_info_*"/BB_DISPLAY_INFO_* on relocation deliberately, for
// low churn -- not a residual bb_info dependency.
// No ESP-IDF or FreeRTOS types here.
// Included by:
//   - platform/espidf/bb_display/bb_display_info.c
//   - platform/host/bb_display/bb_display_info.c
//   - components/bb_display/src/bb_display_info_event_common.c
//   - test/test_host/test_bb_display_info_event.c
//   - test/test_host/test_bb_cache_fidelity.c

#include "bb_json.h"

#include <stdbool.h>

#define BB_DISPLAY_INFO_TOPIC "health.display"

// Owned snapshot struct for bb_cache (owned-struct form).
// panel names like "ek79007", "ssd1306", "ili9341", "st77xx", "mock"
// are well under 32 bytes.
typedef struct {
    bool present;
    char panel[32];
    int  width;
    int  height;
    bool enabled;
} bb_display_snap_t;

// Serializer: writes all fields to obj. Assigned directly to bb_cache_serialize_fn
// with NO cast at registration.
void bb_display_serialize(bb_json_t obj, const void *snap);
