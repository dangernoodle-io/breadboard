#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_cache.h"
#include "bb_display.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_openapi.h"
#include "bb_init.h"
#include "bb_str.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_display_info";
static bool s_registered = false;
static bb_event_topic_t s_topic = NULL;

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
        bb_strlcpy(snap.panel, panel, sizeof(snap.panel));
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
    bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = &snap });
    bb_cache_serialize_into(BB_DISPLAY_INFO_TOPIC, section);
}

// ---------------------------------------------------------------------------
// bb_display_register_info: register bb_cache + /api/info section + topic.
//
// Must be called before bb_info_init freezes the section table (i.e. before
// the regular-tier walk). The bb_event_routes_attach_ex call is intentionally
// NOT done here — bb_event_routes is not yet initialized at consumer-call time
// (ESP_ERR_INVALID_STATE / 259). The attach is deferred to
// bb_display_info_register_init which runs at registry order 4 (after
// bb_event_routes at order 0).
// ---------------------------------------------------------------------------

void bb_display_register_info(void)
{
    // Register owned-struct cache entry first (REST path reads from it).
    bb_cache_config_t cache_cfg = {
        .key       = BB_DISPLAY_INFO_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_display_snap_t),
        .serialize = bb_display_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    bb_err_t cerr = bb_cache_register(&cache_cfg);
    if (cerr != BB_OK) {
        bb_log_w(TAG, "bb_cache_register failed: %d", (int)cerr);
        return;
    }

    bb_info_register_section("display", display_section_get, NULL, k_display_schema);

    // Register retained health.display event topic.
    bb_err_t err = bb_event_topic_register(BB_DISPLAY_INFO_TOPIC, &s_topic);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic register failed: %d", (int)err);
        return;
    }

    bb_openapi_register_topic_schema(BB_DISPLAY_INFO_TOPIC, k_display_schema, "DisplayInfo");

    s_registered = true;
}

// ---------------------------------------------------------------------------
// bb_display_info_register_init: deferred registry-tier init (order 4).
//
// Runs after bb_event_routes_init (order 0) so the attach succeeds.
// Also seeds the initial cache snapshot and publishes via bb_cache_post.
// ---------------------------------------------------------------------------

#if defined(CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH) && CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH

static bb_err_t bb_display_info_register_init(bb_http_handle_t server)
{
    (void)server;
    if (!s_registered) return BB_OK;

    bb_err_t attach_err = bb_event_routes_attach_ex(BB_DISPLAY_INFO_TOPIC, true);
    if (attach_err != BB_OK) {
        bb_log_w(TAG, "auto-attach failed for '" BB_DISPLAY_INFO_TOPIC "': %d",
                 (int)attach_err);
    }

    // Seed the cache with the initial snapshot then post to the event ring.
    bb_display_snap_t snap = make_snap();
    bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = &snap });
    bb_cache_post(BB_DISPLAY_INFO_TOPIC);

    return BB_OK;
}

/* order 4: after bb_event_routes_init (order 0) — mirrors bb_ota_check. */
BB_INIT_REGISTER_N(bb_display_info, bb_display_info_register_init, 4);

#endif /* CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH */
