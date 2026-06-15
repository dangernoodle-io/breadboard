#include "bb_mqtt_info.h"
#include "bb_health.h"
#include "bb_json.h"
#include "bb_mqtt.h"

/* JSON-Schema value for the "mqtt" section contributed to the /api/health 200 schema. */
static const char k_mqtt_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"connected\":{\"type\":\"boolean\"}}}";

static void mqtt_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_mqtt_t h = bb_mqtt_default();
    bool enabled   = (h != NULL);
    bool connected = enabled && bb_mqtt_is_connected(h);
    bb_json_obj_set_bool(section, "enabled",   enabled);
    bb_json_obj_set_bool(section, "connected", connected);
}

void bb_mqtt_register_health(void)
{
    bb_health_register_section("mqtt", mqtt_section_get, NULL, k_mqtt_schema);
}
