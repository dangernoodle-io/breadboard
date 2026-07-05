// Pure (host-testable) JSON builder for the bb_health stack low-water
// observer. No FreeRTOS or ESP-IDF types here.
//
// Task-registry unification PR3: the mark-and-sweep debounce table and
// is_low() decision this file used to own moved to bb_task_registry's
// base-scan job (components/bb_task_registry/
// bb_task_registry_base_scan_common.c) -- bb_health_stack is now a pure
// observer of that scan's low-stack transition callback.
#include "bb_health_stack.h"

#include <stdio.h>

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
