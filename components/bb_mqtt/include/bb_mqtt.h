// bb_mqtt — portable MQTT client component.
//
// Wraps esp-mqtt on ESP-IDF; in-memory stub on host for testing.
// Public header is free of esp_*.h, mqtt_client.h, cJSON, and freertos/*.h
// so the same header compiles unchanged on all backends.
//
// TLS lifetime: when tls=true, bb_mqtt_init resolves credentials via
// bb_tls_creds_resolve and keeps them alive for the client's lifetime.
// bb_tls_creds_free is called only inside bb_mqtt_destroy, after the
// underlying client is torn down.
#pragma once

#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle — never dereference directly.
typedef void *bb_mqtt_t;

/**
 * Configuration for bb_mqtt_init.
 *
 * uri        — MQTT broker URI: mqtt:// or mqtts://host:port
 * client_id  — NULL: default to bb_nv_config_hostname(); "" (empty string):
 *              set_null_client_id (broker assigns ID)
 * username   — optional; NULL to omit
 * password   — optional; NULL to omit
 * lwt_topic  — optional last-will topic; NULL to omit
 * lwt_msg    — last-will payload (ignored when lwt_topic is NULL)
 * tls        — use mqtts TLS; credentials resolved from NVS via bb_tls_creds
 * creds_ns   — NVS namespace for bb_tls_creds_resolve (e.g. "bb_mqtt")
 */
typedef struct {
    const char *uri;
    const char *client_id;
    const char *username;
    const char *password;
    const char *lwt_topic;
    const char *lwt_msg;
    bool        tls;
    const char *creds_ns;
} bb_mqtt_cfg_t;

/**
 * Initialise the MQTT client and start connecting (non-blocking).
 * esp-mqtt owns reconnect internally; callers do not need to retry.
 *
 * @param cfg  Configuration (copied / resolved at call time; caller may free after return)
 * @param out  Receives the opaque handle on success
 * @return BB_OK on success, BB_ERR_INVALID_ARG if cfg or out is NULL,
 *         BB_ERR_NO_SPACE on allocation failure, or a platform error.
 */
bb_err_t bb_mqtt_init(const bb_mqtt_cfg_t *cfg, bb_mqtt_t *out);

/**
 * Publish a message to the broker.
 *
 * @param h       Handle from bb_mqtt_init
 * @param topic   Topic string
 * @param payload Message payload (may be NULL for zero-length message)
 * @param len     Payload length; pass -1 to measure via strlen(payload)
 * @param qos     QoS level (0, 1, or 2)
 * @param retain  Retain flag
 * @return BB_OK on success; BB_ERR_INVALID_STATE if not connected.
 */
bb_err_t bb_mqtt_publish(bb_mqtt_t h, const char *topic, const char *payload,
                          int len, int qos, bool retain);

/**
 * Subscribe to a topic.  Reserved for closed-loop consumers.
 * Only guaranteed on the espidf backend today; host stub returns BB_OK.
 */
bb_err_t bb_mqtt_subscribe(bb_mqtt_t h, const char *topic, int qos);

/** Returns true when the client is currently connected to the broker. */
bool bb_mqtt_is_connected(bb_mqtt_t h);

/**
 * Disconnect, destroy the client, and free all resources including TLS creds.
 * Safe to call with NULL.
 */
bb_err_t bb_mqtt_destroy(bb_mqtt_t h);

/**
 * Return the auto-registered MQTT handle created by the EARLY-tier
 * self-registration (CONFIG_BB_MQTT_AUTOREGISTER=y), or NULL if:
 *   - the feature is compiled out,
 *   - NVS enabled=0 (MQTT disabled by config), or
 *   - bb_mqtt_autoregister_init has not yet run.
 *
 * On the host backend this always returns NULL unless overridden via
 * bb_mqtt_default_set (available when BB_MQTT_TESTING is defined).
 *
 * Callers must treat NULL as "no MQTT" and skip sink wiring gracefully.
 */
bb_mqtt_t bb_mqtt_default(void);

// ---------------------------------------------------------------------------
// Host test hooks (only when BB_MQTT_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_MQTT_TESTING

/** Last published message captured by the host stub. */
typedef struct {
    char  topic[128];
    char  payload[512];
    int   qos;
    bool  retain;
} bb_mqtt_host_pub_t;

/** Return the most recently published message, or NULL if none. */
const bb_mqtt_host_pub_t *bb_mqtt_host_last_pub(bb_mqtt_t h);

/** Number of publish calls since init or last reset. */
int bb_mqtt_host_pub_count(bb_mqtt_t h);

/** Force the connected flag (for testing is_connected). */
void bb_mqtt_host_set_connected(bb_mqtt_t h, bool connected);

/** Reset publish history and connected flag for a handle. */
void bb_mqtt_host_reset(bb_mqtt_t h);

/** Override the handle returned by bb_mqtt_default() for testing. */
void bb_mqtt_default_set(bb_mqtt_t h);

#endif /* BB_MQTT_TESTING */

#ifdef __cplusplus
}
#endif
