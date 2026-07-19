#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_cache.h"

#include <string.h>

// B1-893: re-homed from the deleted bb_display_info satellite -- host stub,
// no event bus. Only the health.display cache registration survives; the
// /api/info "display" section died with the satellite.

void bb_display_register_info(void)
{
    // Register owned-struct cache entry (REST/SSE path reads from it).
    bb_cache_config_t cache_cfg = {
        .key       = BB_DISPLAY_INFO_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_display_snap_t),
        .serialize = bb_display_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_cache_register(&cache_cfg);
    // Host stub: no bb_data touch.
}
