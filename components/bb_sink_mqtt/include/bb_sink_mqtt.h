// bb_sink_mqtt — MQTT sink adapter for bb_pub.
//
// Bridges bb_mqtt_client_publish into a bb_pub_sink_t so the transport-agnostic
// bb_pub core can deliver telemetry over MQTT without depending on bb_mqtt_client.
// Under B1-289 (reboot-to-apply) the handle is wired once at boot and is
// stable for the device lifetime.
//
// Usage:
//   bb_pub_sink_t s;
//   bb_sink_mqtt(bb_mqtt_client_default(), &s);
//   bb_pub_add_sink(&s);
#pragma once

#include "bb_core.h"
#include "bb_mqtt_client.h"
#include "bb_pub.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fill `out` with a sink whose publish() calls:
 *   bb_mqtt_client_publish(h, topic, payload, len, qos, retain)
 *
 * QoS can be overridden at compile time via CONFIG_BB_SINK_MQTT_QOS (default 0).
 * The retain flag is forwarded from bb_pub per-source cfg (bb_pub_telemetry_cfg_t.retain).
 *
 * Sets out->transport = "mqtt" and out->tls = bb_mqtt_client_is_tls(h) at wire time
 * so each published payload carries device-reported transport metadata.
 *
 * The caller owns the bb_pub_sink_t struct and must keep `h` valid for the
 * lifetime of the sink.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if h or out is NULL.
 */
bb_err_t bb_sink_mqtt(bb_mqtt_client_t h, bb_pub_sink_t *out);

/**
 * Fill `out` with a dynamic sink that resolves bb_mqtt_client_default() on EVERY
 * publish call rather than capturing the handle pointer at registration time.
 *
 * This variant survives OTA suspend/resume: bb_mqtt_client_suspend_default() destroys
 * the handle and bb_mqtt_client_resume_default() allocates a NEW one at a different
 * address.  Because mqtt_publish_default() calls bb_mqtt_client_default() each time,
 * it automatically uses the fresh post-resume handle — no re-registration
 * needed.  During the suspend window (bb_mqtt_client_default() == NULL),
 * bb_mqtt_client_publish() returns BB_ERR_INVALID_ARG and no publish occurs — a clean
 * no-op, not a use-after-free crash.
 *
 * Prefer this variant over bb_sink_mqtt() for the autoregistered default
 * client.  Use bb_sink_mqtt() only when you hold an explicit handle that you
 * know is stable for the sink's lifetime.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if out is NULL.
 */
bb_err_t bb_sink_mqtt_default(bb_pub_sink_t *out);

// ---------------------------------------------------------------------------
// Testing hooks (BB_SINK_MQTT_TESTING only)
// ---------------------------------------------------------------------------
#ifdef BB_SINK_MQTT_TESTING
// Reset the lazily-registered bb_transport_health handle so tests that also
// call bb_transport_health_reset_for_test() don't leave the sink holding a
// stale (now-unused) slot index.
void bb_sink_mqtt_reset_transport_health_for_test(void);
#endif /* BB_SINK_MQTT_TESTING */

#ifdef __cplusplus
}
#endif
