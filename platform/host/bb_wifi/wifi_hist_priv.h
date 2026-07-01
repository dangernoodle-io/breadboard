#pragma once
#include <stdint.h>

// Shared private helper: find the top standard reason in a 256-entry histogram.
// Skips breadboard sentinels 99 (WIFI_REASON_BB_LOST_IP),
// 100 (WIFI_REASON_BB_EGRESS_DEAD), and 101 (WIFI_REASON_BB_NO_IP_WATCHDOG).
// Sets *out_count to the highest non-sentinel count found.
// Returns the bucket index (reason code) for that count, or 0 if all are zero.
static inline uint16_t wifi_hist_top_reason(const uint16_t *hist, uint16_t *out_count)
{
    uint16_t top_count = 0;
    uint16_t top_code  = 0;
    for (int i = 0; i < 256; i++) {
        if (i == 99 || i == 100 || i == 101) continue;
        if (hist[i] > top_count) {
            top_count = hist[i];
            top_code  = (uint16_t)i;
        }
    }
    if (out_count) *out_count = top_count;
    return top_code;
}
