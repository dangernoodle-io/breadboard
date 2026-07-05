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

bb_health_stack_entry_t *bb_health_stack_table_mark(bb_health_stack_entry_t *table, int cap,
                                                    const char *name, uint32_t scan_tick)
{
    if (!table || cap <= 0 || !name) return NULL;

    for (int i = 0; i < cap; i++) {
        if (table[i].in_use &&
            strncmp(table[i].name, name, sizeof(table[i].name)) == 0) {
            table[i].seen_tick = scan_tick;
            return &table[i];
        }
    }

    for (int i = 0; i < cap; i++) {
        if (!table[i].in_use) {
            memset(&table[i], 0, sizeof(table[i]));
            strncpy(table[i].name, name, sizeof(table[i].name) - 1);
            table[i].name[sizeof(table[i].name) - 1] = '\0';
            table[i].in_use = true;
            table[i].low = false;
            table[i].seen_tick = scan_tick;
            return &table[i];
        }
    }

    return NULL;  // table full and name not already tracked
}

// Grace window: a scan can transiently omit a live task (uxTaskGetSystemState's
// task count can shift between the count call and the snapshot). An entry
// survives a single missed scan; it is freed only once it has been missed for
// more than BB_HEALTH_STACK_SWEEP_GRACE consecutive scans. A missed-but-not-
// yet-swept entry is left untouched (including its `low` field) -- mark only
// runs for tasks actually seen this scan, so a missed task's entry simply
// isn't re-marked.
#define BB_HEALTH_STACK_SWEEP_GRACE 1

int bb_health_stack_table_sweep(bb_health_stack_entry_t *table, int cap, uint32_t scan_tick)
{
    if (!table || cap <= 0) return 0;

    int freed = 0;
    for (int i = 0; i < cap; i++) {
        if (table[i].in_use && table[i].seen_tick != scan_tick &&
            (scan_tick - table[i].seen_tick) > BB_HEALTH_STACK_SWEEP_GRACE) {
            memset(&table[i], 0, sizeof(table[i]));
            freed++;
        }
    }
    return freed;
}
