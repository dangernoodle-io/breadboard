// bb_sink_mqtt — MQTT sink adapter for bb_pub.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_sink_mqtt.h"
#include "bb_mqtt_client.h"
#include "bb_transport_health.h"

#ifndef CONFIG_BB_SINK_MQTT_QOS
#define CONFIG_BB_SINK_MQTT_QOS 0
#endif

// bb_transport_health registration (observe-only): registered lazily on the
// first publish attempt, since bb_sink_mqtt/bb_sink_mqtt_default are wired
// before the exclusive-sink arbiter has decided this sink is active — only
// the active sink ever publishes, so only the active sink ever registers.
// Shared between the fixed-handle and default-handle variants: only one of
// them is ever wired into bb_pub on a given board (exclusive-sink arbiter).
// Single-writer: sink->publish() is only called from the bb_pub worker task,
// so the lazy check-then-register on s_th_handle needs no lock
// (bb_transport_health's own ops are internally locked).
static bb_transport_handle_t s_th_handle = BB_TRANSPORT_HANDLE_INVALID;

static void report_transport_health(bool ok)
{
    if (s_th_handle == BB_TRANSPORT_HANDLE_INVALID) {
        bb_transport_health_register("mqtt", BB_TRANSPORT_AUTHORITATIVE, &s_th_handle);
    }
    bb_transport_health_report(s_th_handle, ok);
}

// ---------------------------------------------------------------------------
// bb_sink_mqtt — fixed-handle sink
// ---------------------------------------------------------------------------

static bb_err_t mqtt_publish(void *ctx, const char *topic,
                              const char *payload, int len, bool retain)
{
    bb_mqtt_client_t h = (bb_mqtt_client_t)ctx;
    bb_err_t rc = bb_mqtt_client_publish(h, topic, payload, len,
                                  CONFIG_BB_SINK_MQTT_QOS,
                                  retain);
    report_transport_health(rc == BB_OK);
    return rc;
}

bb_err_t bb_sink_mqtt(bb_mqtt_client_t h, bb_pub_sink_t *out)
{
    if (!h || !out) return BB_ERR_INVALID_ARG;
    out->publish       = mqtt_publish;
    out->ctx           = h;
    out->transport     = "mqtt";
    out->tls           = bb_mqtt_client_is_tls(h);
    out->subscribe     = NULL;
    out->subscribe_ctx = NULL;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_sink_mqtt_default — dynamic-handle sink (survives OTA suspend/resume)
// ---------------------------------------------------------------------------

// mqtt_publish_default resolves the handle at publish time via bb_mqtt_client_default()
// so it always uses the current live handle, not a pointer captured at boot.
// If bb_mqtt_client_default() returns NULL (handle suspended during OTA), bb_mqtt_client_publish
// returns BB_ERR_INVALID_ARG — a clean no-op, not a crash.
static bb_err_t mqtt_publish_default(void *ctx, const char *topic,
                                      const char *payload, int len, bool retain)
{
    (void)ctx;
    bb_mqtt_client_t h = bb_mqtt_client_default();
    bb_err_t rc = bb_mqtt_client_publish(h, topic, payload, len,
                                  CONFIG_BB_SINK_MQTT_QOS,
                                  retain);
    report_transport_health(rc == BB_OK);
    return rc;
}

bb_err_t bb_sink_mqtt_default(bb_pub_sink_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    out->publish       = mqtt_publish_default;
    out->ctx           = NULL;
    out->transport     = "mqtt";
    // Best-effort TLS detection at registration time; the flag is informational
    // and does not affect publish behaviour.
    out->tls           = bb_mqtt_client_is_tls(bb_mqtt_client_default());
    out->subscribe     = NULL;
    out->subscribe_ctx = NULL;
    return BB_OK;
}

#ifdef BB_SINK_MQTT_TESTING
void bb_sink_mqtt_reset_transport_health_for_test(void)
{
    s_th_handle = BB_TRANSPORT_HANDLE_INVALID;
}
#endif
