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

// ---------------------------------------------------------------------------
// bb_sink_mqtt — fixed-handle sink
// ---------------------------------------------------------------------------

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
    out->publish   = mqtt_publish;
    out->ctx       = h;
    out->transport = "mqtt";
    out->tls       = bb_mqtt_is_tls(h);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_sink_mqtt_default — dynamic-handle sink (survives OTA suspend/resume)
// ---------------------------------------------------------------------------

// mqtt_publish_default resolves the handle at publish time via bb_mqtt_default()
// so it always uses the current live handle, not a pointer captured at boot.
// If bb_mqtt_default() returns NULL (handle suspended during OTA), bb_mqtt_publish
// returns BB_ERR_INVALID_ARG — a clean no-op, not a crash.
static bb_err_t mqtt_publish_default(void *ctx, const char *topic,
                                      const char *payload, int len)
{
    (void)ctx;
    bb_mqtt_t h = bb_mqtt_default();
    return bb_mqtt_publish(h, topic, payload, len,
                           CONFIG_BB_SINK_MQTT_QOS,
                           (bool)CONFIG_BB_SINK_MQTT_RETAIN);
}

bb_err_t bb_sink_mqtt_default(bb_pub_sink_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    out->publish   = mqtt_publish_default;
    out->ctx       = NULL;
    out->transport = "mqtt";
    // Best-effort TLS detection at registration time; the flag is informational
    // and does not affect publish behaviour.
    out->tls       = bb_mqtt_is_tls(bb_mqtt_default());
    return BB_OK;
}

