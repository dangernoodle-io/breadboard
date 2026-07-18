#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_cache.h"
#include "bb_data.h"
#include "bb_display.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_openapi.h"
#include "bb_http_server.h"
#include "bb_settings.h"
#include "bb_str.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_display";
static bool s_registered = false;

/* JSON-Schema value for the health.display SSE topic. */
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
        bb_strlcpy(snap.panel, panel, sizeof(snap.panel));
        snap.width   = bb_display_width();
        snap.height  = bb_display_height();
        snap.enabled = bb_settings_display_enabled_get();
    }
    return snap;
}

// ---------------------------------------------------------------------------
// bb_display_register_info: register the health.display bb_cache entry +
// OpenAPI topic schema.
//
// Must be called before the deferred registry-tier init below.
//
// B1-893: re-homed from the deleted bb_display_info satellite -- this
// cache/SSE surface is independent of bb_info and stays live. The
// /api/info "display" section (bb_info_register_section) died with the
// satellite; only the cache/openapi registration below survives.
// ---------------------------------------------------------------------------

void bb_display_register_info(void)
{
    // Register owned-struct cache entry first (REST path reads from it).
    // SSE/broadcast delivery is a bb_data/bb_data_http composition-root
    // concern now (B1-1045), not bb_cache's -- BB_CACHE_FLAG_NONE.
    bb_cache_config_t cache_cfg = {
        .key       = BB_DISPLAY_INFO_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_display_snap_t),
        .serialize = bb_display_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t cerr = bb_cache_register(&cache_cfg);
    if (cerr != BB_OK) {
        bb_log_w(TAG, "bb_cache_register failed: %d", (int)cerr);
        return;
    }

    bb_openapi_register_topic_schema(BB_DISPLAY_INFO_TOPIC, k_display_schema, "DisplayInfo");

    s_registered = true;
}

// ---------------------------------------------------------------------------
// bb_display_info_register_init: deferred registry-tier init (order 4).
//
// Seeds the initial cache snapshot and bumps the "health.display" bb_data
// generation (B1-1045) -- attach/wiring to /api/events lives at the
// composition root.
// ---------------------------------------------------------------------------

bb_err_t bb_display_info_register_init(bb_http_handle_t server)
{
    (void)server;
    if (!s_registered) return BB_OK;

    bb_display_snap_t snap = make_snap();
    bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = &snap });
    bb_data_touch(BB_DISPLAY_INFO_TOPIC);

    return BB_OK;
}
