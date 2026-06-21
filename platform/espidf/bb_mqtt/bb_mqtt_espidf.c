// bb_mqtt ESP-IDF backend — wraps esp-mqtt with TLS via bb_tls_creds.
//
// TLS lifetime: bb_tls_creds_t is allocated in the handle and freed only
// inside bb_mqtt_destroy, AFTER esp_mqtt_client_destroy completes.
// esp-mqtt does NOT copy certificate/key buffers, so the pointers must
// remain valid for the entire client lifetime.
//
// EARLY-tier self-registration: reads NVS "bb_mqtt" namespace for
// uri/client_id/username/password/enabled and connects when enabled=1.
// The handle lives for the app lifetime; telemetry init wires it as a
// bb_pub sink at PRE_HTTP time (register-on-enable, B1-289).
//
// Deferred start: esp_mqtt_client_start() is NOT called until the station
// has an IP address.  bb_mqtt_init registers a bb_wifi_on_got_ip callback
// (or starts immediately when bb_wifi_has_ip() is already true).  Only the
// FIRST start is deferred; esp-mqtt's built-in reconnect handles later drops.
//
// Event-handler safety: esp-mqtt dispatches events via the ESP event loop,
// which can hold a queued event after esp_mqtt_client_stop returns.  To
// prevent a stale event firing into a freed handle we set h->destroyed =
// true and NULL h->client BEFORE calling esp_mqtt_client_destroy; the
// event handler checks h->destroyed under h->lock and returns immediately.
#include "bb_mqtt.h"
#include "bb_tls_creds.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_registry.h"
#include "bb_wifi.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bb_mqtt";

#define BB_MQTT_NVS_NS        "bb_mqtt"
#define BB_MQTT_URI_MAX       128
#define BB_MQTT_CLIENT_ID_MAX 64
#define BB_MQTT_USER_MAX      64
#define BB_MQTT_PASS_MAX      64

// ---------------------------------------------------------------------------
// Internal handle
// ---------------------------------------------------------------------------

typedef struct {
    esp_mqtt_client_handle_t client;
    bb_tls_creds_t           creds;     // kept alive while client runs
    SemaphoreHandle_t        lock;
    bool                     connected;
    bool                     ever_connected; // true once MQTT_EVENT_CONNECTED fires
    bool                     started;   // true once esp_mqtt_client_start called
    bool                     destroyed; // set before esp_mqtt_client_destroy;
                                        // guards event handler against stale arg
    bool                     tls;       // captured from cfg.tls at init time
    bool                     suspended; // true while transiently quiesced via
                                        // bb_mqtt_suspend_default; cleared by resume
    uint32_t                 reconnect_count;     // incremented on each reconnect
    uint8_t                  last_disc_error_type; // last MQTT_EVENT_ERROR error_type
} bb_mqtt_handle_t;

// ---------------------------------------------------------------------------
// Deferred-start support
//
// Only one handle can be pending a deferred start at a time (the autoregistered
// client).  When bb_mqtt_init runs at EARLY tier WiFi has not yet acquired an
// IP, so we register a bb_wifi got-IP callback that fires the first start.
// If WiFi already has an IP at init time (e.g. a later manual call), we start
// immediately and skip the callback registration.
// ---------------------------------------------------------------------------

static bb_mqtt_handle_t *s_pending_start = NULL;   // handle waiting for got-IP

static void mqtt_start_once(bb_mqtt_handle_t *h)
{
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool already = h->started;
    if (!already) h->started = true;
    xSemaphoreGive(h->lock);

    if (already) return;

    bb_err_t rc = esp_mqtt_client_start(h->client);
    if (rc != BB_OK) {
        bb_log_e(TAG, "esp_mqtt_client_start failed: %d", rc);
    } else {
        bb_log_i(TAG, "started (deferred until got-ip)");
    }
}

static void on_got_ip_cb(void)
{
    bb_mqtt_handle_t *h = s_pending_start;
    if (h) {
        s_pending_start = NULL;   // clear before start; idempotent if called again
        mqtt_start_once(h);
    }
}

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    bb_mqtt_handle_t *h = (bb_mqtt_handle_t *)arg;

    // Guard: esp-mqtt may dispatch a queued event after esp_mqtt_client_stop
    // returns (the event loop processes its queue independently).  If destroy
    // has already been called we bail immediately to avoid accessing freed
    // memory.  h->lock is still valid here because we delete it AFTER setting
    // destroyed and calling esp_mqtt_client_destroy.
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool dead = h->destroyed;
    xSemaphoreGive(h->lock);
    if (dead) return;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->connected = true;
        h->ever_connected = true;
        xSemaphoreGive(h->lock);
        bb_log_i(TAG, "connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(h->lock, portMAX_DELAY);
        if (h->ever_connected) {
            h->reconnect_count++;
        }
        h->connected = false;
        xSemaphoreGive(h->lock);
        bb_log_i(TAG, "disconnected (reconnect_count=%u)", h->reconnect_count);
        break;
    case MQTT_EVENT_ERROR: {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        xSemaphoreTake(h->lock, portMAX_DELAY);
        if (event->error_handle) {
            h->last_disc_error_type = (uint8_t)event->error_handle->error_type;
        }
        xSemaphoreGive(h->lock);
        bb_log_w(TAG, "mqtt error event");
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_init(const bb_mqtt_cfg_t *cfg, bb_mqtt_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (!cfg->uri || !cfg->uri[0]) return BB_ERR_INVALID_ARG;

    bb_mqtt_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return BB_ERR_NO_SPACE;

    h->lock = xSemaphoreCreateMutex();
    if (!h->lock) {
        free(h);
        return BB_ERR_NO_SPACE;
    }

    // Resolve TLS credentials when requested.
    // Gated on CONFIG_BB_MQTT_TLS_ENABLE (default n = plaintext-only build).
    // When the gate is OFF and cfg->tls is requested, we degrade gracefully:
    // skip creds resolve and proceed plaintext — matching the Arduino-stub
    // pattern used elsewhere (return BB_ERR_UNSUPPORTED only on hard failure).
    if (cfg->tls) {
#if CONFIG_BB_MQTT_TLS_ENABLE
        bb_err_t rc = bb_tls_creds_resolve(cfg->creds_ns, NULL, &h->creds);
        if (rc != BB_OK) {
            bb_log_w(TAG, "tls_creds_resolve failed: %d", rc);
            // Non-fatal: proceed without creds (broker may accept anonymous TLS)
        }
#else
        bb_log_w(TAG, "TLS requested but BB_MQTT_TLS_ENABLE=n; proceeding plaintext");
#endif
    }

    // Determine client_id.
    // NULL  → use hostname (let esp-mqtt use it via client_id string)
    // ""    → set_null_client_id (broker assigns)
    // other → use as-is
    const char *hostname = bb_nv_config_hostname();
    bool set_null = (cfg->client_id != NULL && cfg->client_id[0] == '\0');
    const char *cid = set_null ? NULL
                    : (cfg->client_id ? cfg->client_id : hostname);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = { .uri = cfg->uri },
        },
        .credentials = {
            .client_id         = cid,
            .set_null_client_id = set_null,
            .username          = cfg->username,
            .authentication = {
                .password = cfg->password,
            },
        },
        .session = {
            .keepalive = CONFIG_BB_MQTT_KEEPALIVE,
        },
        .network = {
            .timeout_ms = CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS,
        },
    };

    // Broker TLS verification certificate.
    // Gate: only when BB_MQTT_TLS_ENABLE is on; otherwise h->creds is zeroed
    // and these assignments are skipped (plaintext degradation).
#if CONFIG_BB_MQTT_TLS_ENABLE
    if (cfg->tls && h->creds.ca) {
        mqtt_cfg.broker.verification.certificate     = h->creds.ca;
        mqtt_cfg.broker.verification.certificate_len = h->creds.ca_len;
    }
#endif

    // Mutual TLS: client certificate + key.
    // Additional gate: BB_TLS_MUTUAL_ENABLE (depends on BB_MQTT_TLS_ENABLE).
#if CONFIG_BB_MQTT_TLS_ENABLE && CONFIG_BB_TLS_MUTUAL_ENABLE
    if (cfg->tls && h->creds.cert) {
        mqtt_cfg.credentials.authentication.certificate     = h->creds.cert;
        mqtt_cfg.credentials.authentication.certificate_len = h->creds.cert_len;
    }
    if (cfg->tls && h->creds.key) {
        mqtt_cfg.credentials.authentication.key     = h->creds.key;
        mqtt_cfg.credentials.authentication.key_len = h->creds.key_len;
    }
#endif

    // Last will.
    if (cfg->lwt_topic && cfg->lwt_topic[0]) {
        mqtt_cfg.session.last_will.topic   = cfg->lwt_topic;
        mqtt_cfg.session.last_will.msg     = cfg->lwt_msg ? cfg->lwt_msg : "";
        mqtt_cfg.session.last_will.msg_len = cfg->lwt_msg ? (int)strlen(cfg->lwt_msg) : 0;
        mqtt_cfg.session.last_will.qos     = 0;
        mqtt_cfg.session.last_will.retain  = false;
    }

    h->client = esp_mqtt_client_init(&mqtt_cfg);
    if (!h->client) {
        bb_log_e(TAG, "esp_mqtt_client_init failed");
        bb_tls_creds_free(&h->creds);
        vSemaphoreDelete(h->lock);
        free(h);
        return BB_ERR_INVALID_STATE;
    }

    esp_mqtt_client_register_event(h->client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, h);

    // Defer start until the station has an IP.  Calling esp_mqtt_client_start
    // before lwip DNS is ready causes getaddrinfo → lwip abort() on WROOM-32.
    if (bb_wifi_has_ip()) {
        // Already connected — start immediately.
        bb_err_t rc = esp_mqtt_client_start(h->client);
        if (rc != BB_OK) {
            bb_log_e(TAG, "esp_mqtt_client_start failed: %d", rc);
            esp_mqtt_client_destroy(h->client);
            bb_tls_creds_free(&h->creds);
            vSemaphoreDelete(h->lock);
            free(h);
            return rc;
        }
        h->started = true;
    } else {
        // No IP yet — arm the got-IP hook, then start immediately if WiFi
        // acquired an IP in the narrow window between the bb_wifi_has_ip()
        // check above and now.  bb_wifi_register_on_got_ip fires the callback
        // synchronously when s_has_ip is already true; mqtt_start_once is
        // idempotent (h->started guard) so calling it from both paths is safe.
        s_pending_start = h;
        bb_wifi_register_on_got_ip(on_got_ip_cb);
        // If the immediate-fire path ran, h->started is already true and
        // we skip the deferred log to avoid the misleading message.
        if (!h->started) {
            bb_log_i(TAG, "init: start deferred until got-ip");
        }
    }

    h->tls = cfg->tls;
    bb_log_i(TAG, "init: uri=%s tls=%d", cfg->uri, cfg->tls);
    *out = h;
    return BB_OK;
}

bb_err_t bb_mqtt_publish(bb_mqtt_t handle, const char *topic,
                          const char *payload, int len, int qos, bool retain)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    bb_mqtt_handle_t *h = (bb_mqtt_handle_t *)handle;

    // Defense-in-depth (B1-296): take the lock, check destroyed/client under it,
    // capture the client pointer locally, then release BEFORE the blocking publish
    // call.  Holding the lock across esp_mqtt_client_publish would deadlock the
    // event handler (which also acquires h->lock on MQTT events).
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool dead = h->destroyed || (h->client == NULL);
    esp_mqtt_client_handle_t client = h->client;
    xSemaphoreGive(h->lock);

    if (dead) {
        bb_log_w(TAG, "publish skipped: handle destroyed or client NULL");
        return BB_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(client, topic,
                                          payload, len, qos, (int)retain);
    if (msg_id < 0) {
        bb_log_w(TAG, "publish failed: topic=%s", topic);
        return BB_ERR_INVALID_STATE;
    }
    return BB_OK;
}

bb_err_t bb_mqtt_subscribe(bb_mqtt_t handle, const char *topic, int qos)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    bb_mqtt_handle_t *h = (bb_mqtt_handle_t *)handle;

    // Mirror the publish guard (B1-296): take the lock, check destroyed/client
    // under it, capture a local pointer, then release BEFORE the blocking call.
    // Holding the lock across esp_mqtt_client_subscribe would deadlock the
    // event handler (which also acquires h->lock on MQTT events).
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool dead = h->destroyed || (h->client == NULL);
    esp_mqtt_client_handle_t client = h->client;
    xSemaphoreGive(h->lock);

    if (dead) {
        bb_log_w(TAG, "subscribe skipped: handle destroyed or client NULL");
        return BB_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_subscribe(client, topic, qos);
    return (msg_id < 0) ? BB_ERR_INVALID_STATE : BB_OK;
}

bool bb_mqtt_is_connected(bb_mqtt_t handle)
{
    if (!handle) return false;
    bb_mqtt_handle_t *h = (bb_mqtt_handle_t *)handle;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool c = h->connected;
    xSemaphoreGive(h->lock);
    return c;
}

bb_err_t bb_mqtt_get_stats(bb_mqtt_t handle, bb_mqtt_stats_t *out)
{
    if (!handle || !out) return BB_ERR_INVALID_ARG;
    bb_mqtt_handle_t *h = (bb_mqtt_handle_t *)handle;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    out->reconnect_count      = h->reconnect_count;
    out->last_disc_error_type = h->last_disc_error_type;
    out->connected            = h->connected;
    xSemaphoreGive(h->lock);
    return BB_OK;
}

bool bb_mqtt_is_tls(bb_mqtt_t handle)
{
    if (!handle) return false;
    return ((bb_mqtt_handle_t *)handle)->tls;
}

bb_err_t bb_mqtt_destroy(bb_mqtt_t handle)
{
    if (!handle) return BB_OK;
    bb_mqtt_handle_t *h = (bb_mqtt_handle_t *)handle;
    if (h->client) {
        // Mark destroyed under lock BEFORE stop/destroy so that any event
        // already queued in the event loop (and not yet dispatched when stop
        // returns) finds the guard and returns immediately instead of
        // dereferencing freed memory → InstrFetchProhibited.
        if (h->lock) {
            xSemaphoreTake(h->lock, portMAX_DELAY);
            h->destroyed = true;
            xSemaphoreGive(h->lock);
        }
        // Only stop a client that was actually started; calling
        // esp_mqtt_client_stop on an unstarted client is a no-op in some
        // esp-mqtt versions but triggers internal assertion failures in others.
        if (h->started) {
            esp_mqtt_client_stop(h->client);
        }
        esp_mqtt_client_destroy(h->client);
        h->client = NULL;
    }
    // Free TLS creds AFTER destroying the client (client holds pointers to them).
    bb_tls_creds_free(&h->creds);
    if (h->lock) {
        vSemaphoreDelete(h->lock);
    }
    free(h);
    return BB_OK;
}

// bb_mqtt_stop — stop and destroy the client, clearing the handle.
// Idempotent: safe to call with a NULL or already-destroyed handle.
// Used by telemetry sink teardown (B1-275) to release the client when
// the exclusive sink slot is lost.
//
// SYNC on ESP-IDF (httpd-safe): stop+destroy do NOT run TLS/mbedTLS and
// have a combined stack depth well under 6144 bytes, so they are safe to
// call inline on the httpd thread.  The handle is NULLed before bb_mqtt_destroy
// is called so subsequent calls are idempotent.  This mirrors the inline
// disable path in bb_mqtt_reconfigure (B1-276).
bb_err_t bb_mqtt_stop(bb_mqtt_t *handle_p)
{
    if (!handle_p || !*handle_p) return BB_OK;
    bb_mqtt_destroy(*handle_p);
    *handle_p = NULL;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// EARLY-tier self-registration (B1-289: register-on-enable at boot)
// ---------------------------------------------------------------------------

#if CONFIG_BB_MQTT_AUTOREGISTER

// Module-level handle so the autoregistered client lives for the app lifetime.
//
// HANDLE STABILITY: s_auto_client is a bb_mqtt_t (void *) whose VALUE changes
// across suspend/resume — suspend destroys the heap-allocated handle object
// (frees task + buffers + struct, ~11 KB) and resume recreates it at a NEW
// address.  Any sink that captured the old pointer value holds a use-after-free
// reference after suspend (B1-296).
//
// The telemetry autoregister uses bb_sink_mqtt_default() (NOT bb_sink_mqtt())
// as the bb_pub sink.  bb_sink_mqtt_default() resolves bb_mqtt_default() on
// every publish call rather than caching the pointer at boot.  This means:
//
//  - During the suspend window (s_auto_client == NULL), bb_mqtt_publish()
//    receives NULL and returns BB_ERR_INVALID_ARG — a clean no-op.
//  - After resume the sink automatically targets the new handle; no
//    re-registration is needed.
//  - Callers SHOULD still bracket suspend/resume with bb_pub_pause()/
//    bb_pub_resume() to avoid unnecessary publish attempts during the window,
//    but even without the pause the dynamic sink is safe (no crash).
//
// bb_mqtt_default() returns s_auto_client (NULL when destroyed, non-NULL when
// live).  Callers that snapshot the return value must treat NULL as "suspended".
static bb_mqtt_t s_auto_client = NULL;

// auto_client_create_from_nvs — read NVS config and create the auto-client.
//
// Shared by bb_mqtt_autoregister_init (boot) and bb_mqtt_resume_default (post-
// suspend recreate).  On success, s_auto_client is set to the new handle but
// NOT yet started; the caller is responsible for starting appropriately:
//  - Boot path:   deferred via got-IP callback (handled inside bb_mqtt_init).
//  - Resume path: immediate start (WiFi/IP already up post-boot).
//
// Returns BB_OK on success or when disabled/unconfigured (non-fatal).
// Returns an error code only when bb_mqtt_init itself fails.
static bb_err_t auto_client_create_from_nvs(void)
{
    char enabled_str[4] = "0";
    bb_nv_get_str(BB_MQTT_NVS_NS, "enabled", enabled_str, sizeof(enabled_str), "0");
    if (enabled_str[0] != '1') {
        bb_log_d(TAG, "autoregister: disabled via NVS");
        return BB_OK;
    }

    char uri[BB_MQTT_URI_MAX]             = {0};
    char client_id[BB_MQTT_CLIENT_ID_MAX] = {0};
    char username[BB_MQTT_USER_MAX]       = {0};
    char password[BB_MQTT_PASS_MAX]       = {0};
    char tls_str[4]                       = "0";

    bb_nv_get_str(BB_MQTT_NVS_NS, "uri",       uri,       sizeof(uri),       "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "client_id", client_id, sizeof(client_id), "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "username",  username,  sizeof(username),  "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "password",  password,  sizeof(password),  "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls",       tls_str,   sizeof(tls_str),   "0");

    if (!uri[0]) {
        bb_log_w(TAG, "autoregister: uri not set");
        return BB_OK;
    }

    bb_mqtt_cfg_t cfg = {
        .uri       = uri,
        .client_id = client_id[0] ? client_id : NULL,
        .username  = username[0]  ? username  : NULL,
        .password  = password[0]  ? password  : NULL,
        .tls       = (tls_str[0] == '1'),
        .creds_ns  = BB_MQTT_NVS_NS,
    };

    bb_err_t rc = bb_mqtt_init(&cfg, &s_auto_client);
    if (rc != BB_OK) {
        bb_log_w(TAG, "auto_client_create_from_nvs: bb_mqtt_init failed: %d", rc);
        s_auto_client = NULL;
    }
    return rc;
}

static bb_err_t bb_mqtt_autoregister_init(void)
{
    // Boot init runs on the 8192-byte main task — safe to call directly.
    // auto_client_create_from_nvs also sets up the deferred got-IP start inside
    // bb_mqtt_init when WiFi has no IP yet (the normal EARLY-tier path).
    bb_err_t rc = auto_client_create_from_nvs();
    if (rc != BB_OK) {
        bb_log_w(TAG, "autoregister init failed: %d", rc);
    }
    return BB_OK;  // non-fatal: EARLY walk continues
}

BB_REGISTRY_REGISTER_EARLY(bb_mqtt, bb_mqtt_autoregister_init);

// bb_mqtt_stop_default — stop and destroy the auto-registered client.
// Called by bb_mqtt_telemetry_init when MQTT loses the exclusive-sink slot
// at boot (B1-289: loser teardown without reboot).
// Idempotent: safe to call when s_auto_client is already NULL.
bb_err_t bb_mqtt_stop_default(void)
{
    if (s_pending_start != NULL &&
        s_pending_start == (bb_mqtt_handle_t *)s_auto_client) {
        s_pending_start = NULL;
    }
    return bb_mqtt_stop(&s_auto_client);
}

// bb_mqtt_suspend_default — fully release the auto-client to reclaim ~11 KB of
// heap headroom (esp-mqtt task + send/receive buffers + TLS creds + handle
// struct) during a heap-heavy TLS operation (e.g. a GitHub update-check
// handshake on a heap-tight ESP32-S2 board).
//
// FULL RELEASE: calls bb_mqtt_stop(&s_auto_client) which stops + DESTROYS the
// client and NULLs the handle.  This frees the entire ~11 KB budget, leaving
// the largest_free_block large enough for a concurrent TLS handshake (tested:
// largest_free_block 21504 after suspend vs ~15 KB with stop-only).
//
// Contrast with bb_mqtt_stop_default() which is a permanent disable (never
// resumed).  suspend/resume is a transient bracket.
//
// CALLER CONTRACT: the caller MUST pause publishing (bb_pub_pause) before
// calling suspend and resume publishing (bb_pub_resume) after resume, so that
// no bb_pub tick fires while s_auto_client is NULL.  bb_sink_mqtt reads
// bb_mqtt_default() at publish time, so it picks up NULL during the suspended
// window and returns BB_ERR_INVALID_ARG — which bb_pub logs but does not fatal
// on.  To avoid spurious errors and wasted tick work, callers MUST pause.
//
// Idempotent: no-op + BB_OK if already suspended (s_auto_client already NULL
// and s_suspended_flag set).
static bool s_suspended = false;   // tracks whether we are in a suspend window

bb_err_t bb_mqtt_suspend_default(void)
{
    if (s_suspended) return BB_OK;   // idempotent

    if (s_auto_client) {
        // Clear the pending-start pointer if it still refers to this handle,
        // so the got-IP callback does not fire into the freed handle.
        if (s_pending_start != NULL &&
            s_pending_start == (bb_mqtt_handle_t *)s_auto_client) {
            s_pending_start = NULL;
        }
        bb_mqtt_stop(&s_auto_client);  // destroys + NULLs s_auto_client
    }

    s_suspended = true;
    bb_log_i(TAG, "suspended (full release ~11KB for heap-heavy TLS op)");
    return BB_OK;
}

// bb_mqtt_resume_default — recreate the auto-client from NVS and reconnect
// immediately after a bb_mqtt_suspend_default().
//
// Post-boot the station already has an IP address, so we do NOT use the
// deferred got-IP path.  Instead we call auto_client_create_from_nvs() (which
// runs bb_mqtt_init — this sets up the client and starts it immediately when
// bb_wifi_has_ip() is true, which it will be post-boot).  If for some reason
// WiFi is down, bb_mqtt_init falls back to the deferred got-IP path so the
// behaviour is still safe.
//
// Idempotent: no-op + BB_OK if not suspended.
bb_err_t bb_mqtt_resume_default(void)
{
    if (!s_suspended) return BB_OK;   // idempotent

    bb_err_t rc = auto_client_create_from_nvs();
    if (rc != BB_OK) {
        bb_log_w(TAG, "resume: auto_client_create_from_nvs failed: %d", rc);
        // Do NOT clear s_suspended — let the caller retry.
        return rc;
    }

    s_suspended = false;
    bb_log_i(TAG, "resumed (client recreated from NVS, handle=%p)", s_auto_client);
    return BB_OK;
}

#endif /* CONFIG_BB_MQTT_AUTOREGISTER */

// ---------------------------------------------------------------------------
// Default handle accessor
// ---------------------------------------------------------------------------

bb_mqtt_t bb_mqtt_default(void)
{
#if CONFIG_BB_MQTT_AUTOREGISTER
    return s_auto_client;
#else
    return NULL;
#endif
}

// ---------------------------------------------------------------------------
// bb_mqtt_stop_default — stub when autoregister is disabled
// ---------------------------------------------------------------------------

#if !CONFIG_BB_MQTT_AUTOREGISTER
bb_err_t bb_mqtt_stop_default(void)
{
    // Autoregister disabled; no managed client to stop.
    return BB_OK;
}

bb_err_t bb_mqtt_suspend_default(void)
{
    // Autoregister disabled; no managed client to suspend.
    return BB_OK;
}

bb_err_t bb_mqtt_resume_default(void)
{
    // Autoregister disabled; no managed client to resume.
    return BB_OK;
}
#endif
