#include "bb_wdt.h"

bool bb_wdt_park_wait(bool (*try_wait)(void *ctx, uint32_t ms),
                      void *ctx,
                      uint32_t total_ms,
                      uint32_t slice_ms)
{
    (void)slice_ms;  /* retained for ABI; the parked task is removed, not fed */
    if (!try_wait) return false;

    /* On a single core a parked task cannot get scheduled to feed the WDT
     * while the active task (e.g. the OTA worker) is CPU-bound on cache-
     * disabled flash writes — so feeding from here would still trip the WDT.
     * Instead remove the calling task from the WDT for the duration of the
     * park, then re-add and feed it on resume. The active task's own
     * subscription plus the OTA timeout extension cover the system in the
     * meantime. Assumes the caller is a WDT-subscribed task (mining/asic). */
    bb_wdt_task_unsubscribe();
    bool resumed = try_wait(ctx, total_ms);
    bb_wdt_task_subscribe();
    bb_wdt_task_feed();
    return resumed;
}
