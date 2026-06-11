// Pure (host-testable) decision logic and JSON builder for the bb_health
// stack high-water monitor. No FreeRTOS or ESP-IDF types here.
#include "bb_health_stack.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

bool bb_health_stack_is_low(uint32_t free_bytes, uint32_t threshold)
{
    return free_bytes < threshold;
}

int bb_health_stack_build_json(char *buf, size_t buf_sz,
                               const char *task_name,
                               uint32_t free_bytes,
                               bool low)
{
    if (!buf || buf_sz == 0 || !task_name) return -1;
    int n = snprintf(buf, buf_sz,
        "{\"task\":\"%s\",\"free_bytes\":%" PRIu32 ",\"low\":%s}",
        task_name, free_bytes, low ? "true" : "false");
    return n;
}
