#include "bb_mqtt_info.h"
#include "bb_health.h"
#include "bb_json.h"
#include "bb_mqtt.h"

/* JSON-Schema properties fragment contributed to the /api/health 200 schema. */
static const char k_mqtt_schema_fragment[] =
    "\"mqtt\":{\"type\":\"object\",\"properties\":{"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"connected\":{\"type\":\"boolean\"}}}";

static void mqtt_health_extender(void *root)
{
    bb_mqtt_t h = bb_mqtt_default();
    bool enabled   = (h != NULL);
    bool connected = enabled && bb_mqtt_is_connected(h);

    bb_json_t mqtt = bb_json_obj_new();
    bb_json_obj_set_bool(mqtt, "enabled",   enabled);
    bb_json_obj_set_bool(mqtt, "connected", connected);
    bb_json_obj_set_obj((bb_json_t)root, "mqtt", mqtt);
}

void bb_mqtt_register_health(void)
{
    bb_health_register_extender_ex(mqtt_health_extender, k_mqtt_schema_fragment);
}
