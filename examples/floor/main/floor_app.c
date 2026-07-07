// FOUNDATIONAL FLOOR (jae/floor-handwire, decision #724): the true minimal
// bootable base -- bb_log + while(1) print, HAND-WIRED (a la carte). Self-
// registration is dead; the floor calls each component's init directly and
// does not depend on the bb_init runtime walker at boot. Every layered-up bb
// example should be measurably larger than this floor.
#include "bb_log.h"
#include "bb_meminfo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

static const char *TAG = "floor_app";

void app_main(void)
{
    // Hand-wired in the same order the bb_init EARLY tier previously
    // replayed them via BB_INIT_REGISTER_EARLY (constructor/link order,
    // not the walker): the log stream worker (console writer task + ring
    // buffer) first, then the Kconfig-driven default/per-tag log levels.
    bb_log_stream_init();
    bb_log_config_init();

    uint32_t n = 0;
    while (1) {
        bb_log_i(TAG, "boot tick %" PRIu32, n++);

        // The floor's first telemetry SOURCE (heap): read via bb_meminfo,
        // the canonical heap_caps reader SSOT (KB #698/#699/#693). Measured,
        // not published -- serial only, not yet a bb_pub source.
        bb_meminfo_snapshot_t m;
        bb_meminfo_get(&m);
        bb_log_i(TAG, "heap int_free=%u int_min=%u int_largest=%u spiram_free=%u",
                 (unsigned)m.internal.free, (unsigned)m.internal.min_ever_free,
                 (unsigned)m.internal.largest_free_block, (unsigned)m.spiram.free);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
