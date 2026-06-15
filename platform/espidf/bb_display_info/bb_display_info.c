#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_display.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_registry.h"

#include <stdbool.h>
#include <stddef.h>

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

static void display_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    const char *panel = bb_display_backend_name();

    if (panel) {
        bb_json_obj_set_bool(section, "present", true);
        bb_json_obj_set_string(section, "panel", panel);
        bb_json_obj_set_number(section, "width",  (double)bb_display_width());
        bb_json_obj_set_number(section, "height", (double)bb_display_height());
        bb_json_obj_set_bool(section, "enabled", bb_nv_config_display_enabled());
    } else {
        bb_json_obj_set_bool(section, "present", false);
    }
}

// ---------------------------------------------------------------------------
// bb_display_register_info: register /api/info section + health.display topic.
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
    bb_info_register_section("display", display_section_get, NULL, k_display_schema);

    // Register retained health.display event topic. Must happen early so that
    // any runtime bb_event_post calls before the registry walk still have a
    // valid topic handle.
    bb_err_t err = bb_event_topic_register(BB_DISPLAY_INFO_TOPIC, &s_topic);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic register failed: %d", (int)err);
        return;
    }

    s_registered = true;
}

// ---------------------------------------------------------------------------
// bb_display_info_register_init: deferred registry-tier init (order 4).
//
// Runs after bb_event_routes_init (order 0) so the attach succeeds.
// Also publishes the initial retained snapshot here (not in
// bb_display_register_info) so the ring exists before the first post.
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

    // Publish initial retained snapshot now that the ring is attached.
    const char *panel = bb_display_backend_name();
    char payload[128];
    int n;
    if (panel) {
        n = bb_display_info_event_build_json(payload, sizeof(payload),
                                             true, panel, NULL);
    } else {
        n = bb_display_info_event_build_json(payload, sizeof(payload),
                                             false, NULL, "no backend");
    }
    if (n > 0) {
        size_t sz = (size_t)n < sizeof(payload) ? (size_t)n : sizeof(payload) - 1;
        bb_event_post(s_topic, panel ? 1 : 0, payload, sz);
    }

    return BB_OK;
}

/* order 4: after bb_event_routes_init (order 0) — mirrors bb_update_check. */
BB_REGISTRY_REGISTER_N(bb_display_info, bb_display_info_register_init, 4);

#endif /* CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH */
