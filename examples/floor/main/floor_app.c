// FOUNDATIONAL FLOOR (jae/floor-handwire, decision #724): the true minimal
// bootable base -- bb_log + while(1) print, HAND-WIRED (a la carte). Self-
// registration is dead; the floor calls each component's init directly and
// does not depend on the bb_init runtime walker at boot. Every layered-up bb
// example should be measurably larger than this floor.
#include "bb_log.h"
#include "bb_meminfo.h"
#include "bb_timer.h"
#include <inttypes.h>

static const char *TAG = "floor_app";

// Heap baseline interval (ms). One-off Kconfig knob would be overkill for
// the floor's single hand-wired job; a #define matches the floor's
// hand-wired-not-registry-driven ethos.
#define FLOOR_HEAP_LOG_INTERVAL_MS 5000

// The floor's first telemetry SOURCE (heap): read via bb_meminfo, the
// canonical heap_caps reader SSOT (KB #698/#699/#693), formatted via
// bb_meminfo_format (the canonical HEAP-ONLY line). Measured, not
// published -- serial only, not yet wired to a telemetry sink. Runs as a
// bb_timer MODE-A job on the shared bb_timer_disp task -- no dedicated task.
static void heap_log_tick(void *arg)
{
    (void)arg;
    bb_memreport_snapshot_t m;
    bb_memreport_get(&m);
    char line[128];
    bb_memreport_format(&m, line, sizeof(line));
    bb_log_i(TAG, "%s", line);
}

void app_main(void)
{
    // Hand-wired in the same order the bb_init EARLY tier previously
    // replayed them via BB_INIT_REGISTER_EARLY (constructor/link order,
    // not the walker): the log stream worker (console writer task + ring
    // buffer) first, then the Kconfig-driven default/per-tag log levels.
    bb_log_stream_init();
    bb_log_config_init();

    bb_log_i(TAG, "boot");

    // Boot-baseline line immediately, then hand off to the periodic job.
    heap_log_tick(NULL);

    bb_periodic_timer_t heap_log_timer = NULL;
    bb_err_t err = bb_timer_deferred_periodic_create(
        heap_log_tick, NULL, "heap_log", &heap_log_timer);
    if (err == BB_OK) {
        bb_timer_periodic_start(heap_log_timer,
                                (uint64_t)FLOOR_HEAP_LOG_INTERVAL_MS * 1000ULL);
    } else {
        bb_log_w(TAG, "heap_log: timer create failed (%d)", (int)err);
    }

    // app_main returns; heap-log job runs on the bb_timer_disp task.
}
