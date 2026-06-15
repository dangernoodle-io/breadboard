// Pure (host-testable) JSON builder for the bb_diag diag.boot retained
// event topic. No FreeRTOS or ESP-IDF types here.
#include "bb_diag_event_priv.h"

#include <stdio.h>
#include <inttypes.h>

int bb_diag_boot_build_json(char *buf, size_t buf_sz,
                             const char *reset_reason,
                             uint32_t abnormal_reset_count,
                             bool panic_available,
                             bool rolled_back)
{
    if (!buf || buf_sz == 0 || !reset_reason) return -1;
    return snprintf(buf, buf_sz,
        "{\"reset_reason\":\"%s\","
        "\"wdt_resets\":%" PRIu32 ","
        "\"panic_available\":%s,"
        "\"rolled_back\":%s}",
        reset_reason,
        abnormal_reset_count,
        panic_available ? "true" : "false",
        rolled_back ? "true" : "false");
}
