#include "bb_led_info.h"
#include "bb_led.h"
#include "bb_info.h"
#include "bb_json.h"

#if defined(CONFIG_BB_LED_INFO_AUTOREGISTER) && CONFIG_BB_LED_INFO_AUTOREGISTER
#include "bb_registry.h"
#endif

/* JSON-Schema value for the "led" section. */
static const char k_led_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"type\":{\"type\":[\"string\",\"null\"]},"
    "\"count\":{\"type\":\"integer\"},"
    "\"rgb\":{\"type\":\"boolean\"},"
    "\"enabled\":{\"type\":\"boolean\"}}}";

static void led_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_led_handle_t primary = bb_led_primary();

    if (primary) {
        bb_json_obj_set_bool(section, "present", true);
        bb_json_obj_set_string(section, "type", bb_led_name(primary));
        bb_json_obj_set_number(section, "count", (double)bb_led_count(primary));
        bb_json_obj_set_bool(section, "rgb",
                             (bb_led_caps(primary) & BB_LED_CAP_RGB) != 0);
        bb_json_obj_set_bool(section, "enabled", bb_led_enabled(primary));
    } else {
        bb_json_obj_set_bool(section, "present", false);
    }
}

void bb_led_register_info(void)
{
    bb_info_register_section("led", led_section_get, NULL, k_led_schema);
}

#if defined(CONFIG_BB_LED_INFO_AUTOREGISTER) && CONFIG_BB_LED_INFO_AUTOREGISTER

/* order 1: bb_info registers at order 2 but its section table accepts
 * registrations at any regular-tier order before bb_info_freeze (order 20).
 * Mirrors the sequencing of the manual call (before bb_registry_init). */
static bb_err_t bb_led_info_autoregister_init(bb_http_handle_t server)
{
    (void)server;
    bb_led_register_info();
    return BB_OK;
}

BB_REGISTRY_REGISTER_N(bb_led_info, bb_led_info_autoregister_init, 1);

#endif /* CONFIG_BB_LED_INFO_AUTOREGISTER */
