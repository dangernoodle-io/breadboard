#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_display.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"

#include <stddef.h>

static const char *TAG = "bb_display_info";

/* JSON-Schema properties fragment contributed to the /api/info 200 schema. */
static const char k_display_schema_fragment[] =
    "\"display\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"panel\":{\"type\":[\"string\",\"null\"]},"
    "\"width\":{\"type\":\"integer\"},"
    "\"height\":{\"type\":\"integer\"},"
    "\"enabled\":{\"type\":\"boolean\"}}}";

static void display_info_extender(bb_json_t root)
{
    const char *panel = bb_display_backend_name();
    bb_json_t disp = bb_json_obj_new();

    if (panel) {
        bb_json_obj_set_bool(disp, "present", true);
        bb_json_obj_set_string(disp, "panel", panel);
        bb_json_obj_set_number(disp, "width",  (double)bb_display_width());
        bb_json_obj_set_number(disp, "height", (double)bb_display_height());
        bb_json_obj_set_bool(disp, "enabled", bb_nv_config_display_enabled());
    } else {
        bb_json_obj_set_bool(disp, "present", false);
    }

    bb_json_obj_set_obj(root, "display", disp);
}

// ---------------------------------------------------------------------------
// bb_display_register_info: register /api/info extender + health.display topic
// ---------------------------------------------------------------------------

void bb_display_register_info(void)
{
    bb_info_register_extender_ex(display_info_extender, k_display_schema_fragment);

    // Register retained health.display event topic.
    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_register(BB_DISPLAY_INFO_TOPIC, &topic);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic register failed: %d", (int)err);
        return;
    }

#if defined(CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH) && CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH
    {
        bb_err_t attach_err = bb_event_routes_attach_ex(BB_DISPLAY_INFO_TOPIC, true);
        if (attach_err != BB_OK) {
            bb_log_w(TAG, "auto-attach failed for '" BB_DISPLAY_INFO_TOPIC "': %d",
                     (int)attach_err);
        }
    }
#endif

    // Publish initial retained snapshot.
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
        bb_event_post(topic, panel ? 1 : 0, payload, sz);
    }
}
