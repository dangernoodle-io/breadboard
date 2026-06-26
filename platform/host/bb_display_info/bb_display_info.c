#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_cache.h"
#include "bb_display.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"

#include <string.h>

static const char *TAG = "bb_display_info";

/* JSON-Schema value for the "display" section. */
static const char k_display_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"panel\":{\"type\":[\"string\",\"null\"]},"
    "\"width\":{\"type\":\"integer\"},"
    "\"height\":{\"type\":\"integer\"},"
    "\"enabled\":{\"type\":\"boolean\"}}}";

static bb_display_snap_t make_snap(void)
{
    bb_display_snap_t snap = {0};
    const char *panel = bb_display_backend_name();
    snap.present = (panel != NULL);
    if (panel) {
        strncpy(snap.panel, panel, sizeof(snap.panel) - 1);
        snap.width   = bb_display_width();
        snap.height  = bb_display_height();
        snap.enabled = bb_nv_config_display_enabled();
    }
    return snap;
}

static void display_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_display_snap_t snap = make_snap();
    bb_cache_update(BB_DISPLAY_INFO_TOPIC, &snap);
    bb_cache_serialize_into(BB_DISPLAY_INFO_TOPIC, section);
}

void bb_display_register_info(void)
{
    // Register owned-struct cache entry (REST path reads from it).
    bb_err_t cerr = bb_cache_register(BB_DISPLAY_INFO_TOPIC, NULL,
                                      sizeof(bb_display_snap_t),
                                      bb_display_serialize);
    if (cerr != BB_OK) {
        bb_log_w(TAG, "bb_cache_register failed: %d", (int)cerr);
        return;
    }

    bb_info_register_section("display", display_section_get, NULL, k_display_schema);
    // Host stub: no event bus.
}
