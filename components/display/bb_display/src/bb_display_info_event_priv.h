#pragma once
// Private shared header: bb_cache snapshot struct for the health.display
// retained event topic, owned by bb_display itself (B1-893 -- re-homed from
// the deleted bb_display_info satellite; this cache/SSE surface is
// independent of bb_info and stays live). Names kept as
// "bb_display_info_*"/BB_DISPLAY_INFO_* on relocation deliberately, for low
// churn -- not a residual bb_info dependency.
//
// bb_display_serialize() (the legacy bb_json bb_cache serializer this header
// used to declare, components/display/bb_display/src/bb_display_info_event_
// common.c) was DELETED in B1-1146a -- this key now self-binds to bb_data
// instead (bb_display_info_bind(), bb_display_info_wire.c) against
// bb_display_info_wire_desc. health.display's REST exposure is being
// rehomed to system.display under bb_system's diag endpoint (B1-1150) --
// that reader will render via bb_data_render() against this same bind.
//
// No ESP-IDF or FreeRTOS types here.
// Included by:
//   - platform/espidf/bb_display/bb_display_info.c
//   - platform/host/bb_display/bb_display_info.c
//   - components/display/bb_display/src/bb_display_info_wire.c
//   - test/test_host/test_bb_display_info_event.c

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
