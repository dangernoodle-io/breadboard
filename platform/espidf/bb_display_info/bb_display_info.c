#include "bb_display_info.h"
#include "bb_display.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_nv.h"

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

void bb_display_register_info(void)
{
    bb_info_register_extender_ex(display_info_extender, k_display_schema_fragment);
}
