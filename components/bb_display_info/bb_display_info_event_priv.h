#pragma once
// Private shared header: pure JSON builder for the bb_display_info
// health.display retained event topic. No ESP-IDF or FreeRTOS types here.
// Included by:
//   - platform/espidf/bb_display_info/bb_display_info.c (emit on attach)
//   - platform/host/bb_display_info/bb_display_info.c (stub + test harness)
//   - components/bb_display_info/bb_display_info_event_common.c (pure impl)

#include <stddef.h>
#include <stdbool.h>

#define BB_DISPLAY_INFO_TOPIC "health.display"

// Write JSON payload for a health.display event into buf[buf_sz].
// Format when present: {"present":true,"panel":"<name>"}
// Format when absent:  {"present":false,"reason":"<reason>"}
// panel may be NULL (only used when present=true).
// reason may be NULL (only used when present=false; if NULL, omitted).
// Returns number of chars that would have been written (like snprintf), -1 on bad args.
int bb_display_info_event_build_json(char *buf, size_t buf_sz,
                                     bool present,
                                     const char *panel,
                                     const char *reason);
