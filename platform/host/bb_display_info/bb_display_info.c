#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_display.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_nv.h"

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

void bb_display_register_info(void)
{
    bb_info_register_section("display", display_section_get, NULL, k_display_schema);
    // Host stub: no event bus; the pure builder is exercised via direct tests.
}
