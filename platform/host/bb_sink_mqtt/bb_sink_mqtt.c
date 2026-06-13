// bb_sink_mqtt — MQTT sink adapter for bb_pub.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_sink_mqtt.h"
#include "bb_mqtt.h"

#ifndef CONFIG_BB_SINK_MQTT_QOS
#define CONFIG_BB_SINK_MQTT_QOS 0
#endif
#ifndef CONFIG_BB_SINK_MQTT_RETAIN
#define CONFIG_BB_SINK_MQTT_RETAIN 0
#endif

static bb_err_t mqtt_publish(void *ctx, const char *topic,
                              const char *payload, int len)
{
    bb_mqtt_t h = (bb_mqtt_t)ctx;
    return bb_mqtt_publish(h, topic, payload, len,
                           CONFIG_BB_SINK_MQTT_QOS,
                           (bool)CONFIG_BB_SINK_MQTT_RETAIN);
}

bb_err_t bb_sink_mqtt(bb_mqtt_t h, bb_pub_sink_t *out)
{
    if (!h || !out) return BB_ERR_INVALID_ARG;
    out->publish = mqtt_publish;
    out->ctx     = h;
    return BB_OK;
}
