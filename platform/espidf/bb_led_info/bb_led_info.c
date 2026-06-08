#include "bb_led_info.h"
#include "bb_led.h"
#include "bb_info.h"
#include "bb_json.h"

/* JSON-Schema properties fragment contributed to the /api/info 200 schema. */
static const char k_led_schema_fragment[] =
    "\"led\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"type\":{\"type\":[\"string\",\"null\"]},"
    "\"count\":{\"type\":\"integer\"},"
    "\"rgb\":{\"type\":\"boolean\"},"
    "\"enabled\":{\"type\":\"boolean\"}}}";

static void led_info_extender(bb_json_t root)
{
    bb_led_handle_t primary = bb_led_primary();
    bb_json_t led = bb_json_obj_new();

    if (primary) {
        bb_json_obj_set_bool(led, "present", true);
        bb_json_obj_set_string(led, "type", bb_led_name(primary));
        bb_json_obj_set_number(led, "count", (double)bb_led_count(primary));
        bb_json_obj_set_bool(led, "rgb",
                             (bb_led_caps(primary) & BB_LED_CAP_RGB) != 0);
        bb_json_obj_set_bool(led, "enabled", bb_led_enabled(primary));
    } else {
        bb_json_obj_set_bool(led, "present", false);
    }

    bb_json_obj_set_obj(root, "led", led);
}

void bb_led_register_info(void)
{
    bb_info_register_extender_ex(led_info_extender, k_led_schema_fragment);
}
