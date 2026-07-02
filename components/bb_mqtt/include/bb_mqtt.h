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
#include "bb_tls.h"
#include "bb_nv_namespaces.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle — never dereference directly.
typedef void *bb_mqtt_t;

/**
 * Coarse classification of the most recent MQTT disconnect cause.
 * Portable — valid on espidf; NONE/0 on arduino/host (unless set via test hook).
 * Mirrors esp_mqtt_error_type_t coarsely; host-safe.
 */
typedef enum {
    BB_MQTT_DISC_NONE              = 0,  // no disconnect observed
    BB_MQTT_DISC_TRANSPORT,              // MQTT_ERROR_TYPE_TCP_TRANSPORT
    BB_MQTT_DISC_CONNECTION_REFUSED,     // MQTT_ERROR_TYPE_CONNECTION_REFUSED
    BB_MQTT_DISC_OTHER,                  // any other error_type
} bb_mqtt_disc_t;

/**
 * Snapshot of MQTT connection statistics.
 *
 * reconnect_count — number of reconnects since init (incremented on
 *                   MQTT_EVENT_DISCONNECTED when the client had previously
 *                   connected at least once).
 * connected       — current connected state (same as bb_mqtt_is_connected).
 * disc_reason     — classified disconnect reason (B1-362).
 * tls_fail        — portable TLS handshake failure classification (B1-362).
 * tls_error_code  — raw mbedtls error code (diagnostic; 0 when none).
 */
typedef struct {
    uint32_t       reconnect_count;
    bool           connected;
    bb_mqtt_disc_t disc_reason;     // REPLACES last_disc_error_type
    bb_tls_fail_t  tls_fail;        // portable TLS handshake classification
    int            tls_error_code;  // raw mbedtls code (diagnostic; 0 when none)
} bb_mqtt_stats_t;

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
 * Return a snapshot of MQTT connection statistics.
 *
 * All fields are read under h->lock, providing a consistent snapshot.
 *
 * @param h    Handle from bb_mqtt_init.  NULL → all fields zeroed, BB_ERR_INVALID_ARG.
 * @param out  Receives the statistics snapshot on success.
 * @return BB_OK on success; BB_ERR_INVALID_ARG if h or out is NULL.
 */
bb_err_t bb_mqtt_get_stats(bb_mqtt_t h, bb_mqtt_stats_t *out);

/**
 * Returns true when the client was initialised with TLS (cfg.tls = true).
 * Safe to call from any context after bb_mqtt_init.
 * Returns false for NULL handles.
 */
bool bb_mqtt_is_tls(bb_mqtt_t h);

/**
 * Disconnect, destroy the client, and free all resources including TLS creds.
 * Safe to call with NULL.
 */
bb_err_t bb_mqtt_destroy(bb_mqtt_t h);

/**
 * Stop and destroy the client via a pointer-to-handle, setting the handle to
 * NULL on return.  Idempotent: safe to call with a NULL handle_p or when
 * *handle_p is already NULL.
 *
 * Intended for telemetry sink teardown (B1-275): when the exclusive-sink
 * slot is released the sink calls bb_mqtt_stop(&s_auto_client) to ensure
 * the client is fully shut down and the handle is cleared atomically.
 *
 * SYNC on ESP-IDF (httpd-safe, when CONFIG_BB_MQTT_AUTOREGISTER is enabled):
 * stop+destroy do NOT run TLS/mbedTLS and are safe to call inline on the
 * httpd thread (6144-byte stack).  The handle pointer is NULLed before
 * bb_mqtt_destroy is called so subsequent calls are idempotent.  When
 * autoregister is disabled (lock not created), the work also runs
 * synchronously — acceptable from the 8192-byte EARLY init path.
 *
 * @param handle_p  Pointer to the handle to stop.  Set to NULL on return.
 * @return BB_OK always (destroy errors are logged but not propagated).
 */
bb_err_t bb_mqtt_stop(bb_mqtt_t *handle_p);

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

/**
 * Stop and destroy the auto-registered (default) MQTT client.
 *
 * Called by bb_mqtt_telemetry_init when MQTT loses the exclusive-sink slot
 * at boot (B1-289 loser-teardown without reboot).  Cancels any pending
 * deferred start and calls bb_mqtt_stop on the internal auto-client pointer.
 *
 * Idempotent: safe to call when the client is already NULL or when
 * autoregister is compiled out.
 *
 * @return BB_OK always.
 */
bb_err_t bb_mqtt_stop_default(void);

/**
 * Quiesce the auto-registered MQTT connection to free heap headroom for a
 * heap-heavy operation such as a TLS GitHub update-check handshake on tight
 * no-PSRAM boards.
 *
 * Two modes, controlled at compile time by CONFIG_BB_MQTT_SUSPEND_STOP_ONLY:
 *
 *   Default (CONFIG_BB_MQTT_SUSPEND_STOP_ONLY=n):
 *     Full release — calls esp_mqtt_client_stop() + esp_mqtt_client_destroy(),
 *     frees TLS credentials, and NULLs the auto-client pointer (~11 KB freed).
 *     Resume recreates from NVS via auto_client_create_from_nvs().
 *
 *   Stop-only (CONFIG_BB_MQTT_SUSPEND_STOP_ONLY=y):
 *     Calls esp_mqtt_client_stop() only.  Keeps the client handle, task, and
 *     buffers resident (~11 KB residency).  The auto-client pointer remains
 *     NON-NULL.  Resume calls esp_mqtt_client_start() on the same handle —
 *     no destroy, no realloc, no NVS reload.  ONLY safe when bb_arena_tls
 *     already reserves enough headroom for the TLS handshake.
 *
 * Contrast with bb_mqtt_stop_default() (permanent teardown, no resume).
 * This function is for a transient pause-and-resume bracket.
 *
 * CALLER CONTRACT: bracket with bb_pub_pause() / bb_pub_resume() to avoid
 * spurious publish attempts during the suspended window.
 *
 * Idempotent: no-op + BB_OK if already suspended, no auto-client exists,
 * or if autoregister is compiled out.
 *
 * @return BB_OK on success or when already suspended; platform error on
 *         esp_mqtt_client_stop failure.
 */
bb_err_t bb_mqtt_suspend_default(void);

/**
 * Re-establish the auto-registered MQTT connection after a
 * bb_mqtt_suspend_default() call.
 *
 * Two modes, controlled at compile time by CONFIG_BB_MQTT_SUSPEND_STOP_ONLY:
 *
 *   Default (CONFIG_BB_MQTT_SUSPEND_STOP_ONLY=n):
 *     Recreates the client from NVS via auto_client_create_from_nvs() and
 *     starts it.  Post-boot the station already has an IP so start is
 *     immediate; if WiFi is down, falls back to the deferred got-IP path.
 *
 *   Stop-only (CONFIG_BB_MQTT_SUSPEND_STOP_ONLY=y):
 *     Calls esp_mqtt_client_start() on the resident handle.  No NVS read,
 *     no realloc.  esp-mqtt reconnects from the existing config.
 *
 * Idempotent: no-op + BB_OK if not currently suspended, or if no auto-client
 * exists, or if autoregister is compiled out.
 *
 * @return BB_OK on success or when not suspended; platform error on
 *         esp_mqtt_client_start failure (suspended flag is preserved for retry).
 */
bb_err_t bb_mqtt_resume_default(void);

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

/**
 * Simulate a reconnect event: if the handle has ever been connected,
 * increments reconnect_count (mirrors the ESP-IDF DISCONNECTED path).
 * If ever_connected is false, sets it to true first so subsequent calls
 * will increment.
 */
void bb_mqtt_host_simulate_reconnect(bb_mqtt_t h);

/** Set the disc_reason field (mirrors MQTT_EVENT_ERROR classification path). */
void bb_mqtt_host_set_disc_reason(bb_mqtt_t h, bb_mqtt_disc_t reason);

/** Set the tls_fail field for testing. */
void bb_mqtt_host_set_tls_fail(bb_mqtt_t h, bb_tls_fail_t fail);

/** Set the tls_error_code field for testing. */
void bb_mqtt_host_set_tls_error_code(bb_mqtt_t h, int code);

/** Override the handle returned by bb_mqtt_default() for testing. */
void bb_mqtt_default_set(bb_mqtt_t h);

/**
 * Returns true if bb_mqtt_suspend_default() has been called and the default
 * handle's suspended flag is set.  Used by tests to assert suspend/resume
 * state without touching internals.  Returns false when no default is set.
 */
bool bb_mqtt_host_is_suspended_default(void);

/**
 * Switch the host stub's suspend/resume behavior between full-release (false,
 * default) and stop-only (true).  Mirrors CONFIG_BB_MQTT_SUSPEND_STOP_ONLY on
 * device.  Must be called before bb_mqtt_suspend_default().
 *
 * stop-only=false (default): suspend destroys handle (NULLs s_default_handle);
 *   resume recreates a fresh handle.
 * stop-only=true: suspend keeps handle resident (s_default_handle NON-NULL),
 *   marks connected=false; resume sets connected=true on same pointer.
 */
void bb_mqtt_host_set_stop_only(bool stop_only);

/**
 * Override the calloc function used by bb_mqtt_init (for alloc-failure testing).
 * Pass NULL to restore the default libc calloc.
 */
void bb_mqtt_set_calloc(void *(*fn)(size_t, size_t));

#endif /* BB_MQTT_TESTING */

#ifdef __cplusplus
}
#endif
