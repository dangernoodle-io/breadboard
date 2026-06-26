#pragma once
// Private shared header: bb_cache snapshot struct and serializer for the
// bb_display_info health.display retained event topic.
// No ESP-IDF or FreeRTOS types here.
// Included by:
//   - platform/espidf/bb_display_info/bb_display_info.c
//   - platform/host/bb_display_info/bb_display_info.c
//   - components/bb_display_info/bb_display_info_event_common.c
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
