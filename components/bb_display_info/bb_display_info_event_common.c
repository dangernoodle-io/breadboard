// Pure (host-testable) JSON builder for the bb_display_info health.display
// retained event topic. No FreeRTOS or ESP-IDF types here.
#include "bb_display_info_event_priv.h"

#include <stdio.h>
#include <stddef.h>

int bb_display_info_event_build_json(char *buf, size_t buf_sz,
                                     bool present,
                                     const char *panel,
                                     const char *reason)
{
    if (!buf || buf_sz == 0) return -1;

    if (present) {
        // panel must be provided when present=true
        if (!panel) return -1;
        return snprintf(buf, buf_sz,
            "{\"present\":true,\"panel\":\"%s\"}", panel);
    }

    // present=false
    if (reason) {
        return snprintf(buf, buf_sz,
            "{\"present\":false,\"reason\":\"%s\"}", reason);
    }
    return snprintf(buf, buf_sz, "{\"present\":false}");
}
