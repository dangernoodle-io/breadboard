#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_display_info_wire.h"
#include "bb_cache.h"
#include "bb_log.h"

#include <string.h>

// B1-893: re-homed from the deleted bb_display_info satellite -- host stub,
// no event bus. Only the health.display cache registration survives; the
// /api/info "display" section died with the satellite.

static const char *TAG = "bb_display";

void bb_display_register_info(void)
{
    // Register owned-struct cache entry (bb_display_info_gather() reads it
    // via bb_cache_get_raw()). cfg->serialize is intentionally omitted
    // (B1-1146a: the legacy bb_json bb_cache serializer,
    // bb_display_serialize(), is deleted -- health.display is being rehomed
    // to system.display under bb_system's diag endpoint, B1-1150, which is
    // where the REST read lives going forward, not here).
    bb_cache_config_t cache_cfg = {
        .key       = BB_DISPLAY_INFO_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_display_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t cerr = bb_cache_register(&cache_cfg);
    if (cerr != BB_OK) {
        bb_log_w(TAG, "bb_cache_register failed: %d", (int)cerr);
        return;
    }

    // Bind "health.display" to bb_data (B1-1146a) -- see the espidf
    // implementation's identical call site (platform/espidf/bb_display/
    // bb_display_info.c) for the full rationale. Non-fatal: logged only.
    bb_err_t derr = bb_display_info_bind();
    if (derr != BB_OK) {
        bb_log_w(TAG, "bb_display_info_bind failed: %d", (int)derr);
    }
    // Host stub: no bb_data touch (no event bus here).
}
