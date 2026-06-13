// bb_mqtt ESP-IDF backend — wraps esp-mqtt with TLS via bb_tls_creds.
//
// TLS lifetime: bb_tls_creds_t is allocated in the handle and freed only
// inside bb_mqtt_destroy, AFTER esp_mqtt_client_destroy completes.
// esp-mqtt does NOT copy certificate/key buffers, so the pointers must
// remain valid for the entire client lifetime.
//
// EARLY-tier self-registration: reads NVS "bb_mqtt" namespace for
// uri/client_id/username/password/enabled and connects when enabled=1.
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
//
// Off-thread reconfigure: bb_mqtt_reconfigure() and bb_mqtt_stop() MUST NOT
// run heavy esp-mqtt lifecycle work (stop/destroy/init/start + mbedTLS) on
// the caller's thread.  The httpd worker stack is only 6144 bytes; the
// esp-mqtt teardown→re-init→connect chain (mbedTLS setup) overflows it.
// Both functions spawn a one-shot FreeRTOS task ("mqtt_reconf", 8192-byte
// stack) that executes the heavy work asynchronously and then self-deletes.
// The caller receives BB_OK immediately; `connected` flips asynchronously
// once the broker handshake completes (visible via bb_mqtt_is_connected and
// GET /api/telemetry).
//
// Reentrancy: s_reconfig_lock is held for the lifetime of the worker task so
// a rapid second PATCH/stop call finds the mutex taken and returns BB_OK
// immediately (coalesced — the in-flight task already owns the reconfigure).
// UAF prevention: the worker captures all state it needs before returning so
// no handle can be freed out from under it.
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
    bool                     started;   // true once esp_mqtt_client_start called
    bool                     destroyed; // set before esp_mqtt_client_destroy;
                                        // guards event handler against stale arg
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
        xSemaphoreGive(h->lock);
        bb_log_i(TAG, "connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->connected = false;
        xSemaphoreGive(h->lock);
        bb_log_i(TAG, "disconnected");
        break;
    case MQTT_EVENT_ERROR:
        bb_log_w(TAG, "mqtt error event");
        break;
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
    if (cfg->tls) {
        bb_err_t rc = bb_tls_creds_resolve(cfg->creds_ns, NULL, &h->creds);
        if (rc != BB_OK) {
            bb_log_w(TAG, "tls_creds_resolve failed: %d", rc);
            // Non-fatal: proceed without creds (broker may accept anonymous TLS)
        }
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
    };

    // Broker TLS verification certificate.
    if (cfg->tls && h->creds.ca) {
        mqtt_cfg.broker.verification.certificate     = h->creds.ca;
        mqtt_cfg.broker.verification.certificate_len = h->creds.ca_len;
    }

    // Mutual TLS: client certificate + key.
    if (cfg->tls && h->creds.cert) {
        mqtt_cfg.credentials.authentication.certificate     = h->creds.cert;
        mqtt_cfg.credentials.authentication.certificate_len = h->creds.cert_len;
    }
    if (cfg->tls && h->creds.key) {
        mqtt_cfg.credentials.authentication.key     = h->creds.key;
        mqtt_cfg.credentials.authentication.key_len = h->creds.key_len;
    }

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

    bb_log_i(TAG, "init: uri=%s tls=%d", cfg->uri, cfg->tls);
    *out = h;
    return BB_OK;
}

bb_err_t bb_mqtt_publish(bb_mqtt_t handle, const char *topic,
                          const char *payload, int len, int qos, bool retain)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    bb_mqtt_handle_t *h = (bb_mqtt_handle_t *)handle;

    int msg_id = esp_mqtt_client_publish(h->client, topic,
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
    int msg_id = esp_mqtt_client_subscribe(h->client, topic, qos);
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
// ASYNC on ESP-IDF: the heavy esp_mqtt_client_stop+destroy work is
// routed through the same one-shot "mqtt_reconf" task as bb_mqtt_reconfigure
// so callers on stack-constrained threads (e.g. httpd, 6144 bytes) are safe.
// The handle is NULLed immediately so subsequent calls are idempotent;
// the underlying client resources are freed asynchronously.
//
// If CONFIG_BB_MQTT_AUTOREGISTER is enabled, bb_mqtt_stop uses the shared
// s_reconfig_lock reentrancy guard (see bb_mqtt_reconfigure).  When called
// outside that context (s_reconfig_lock == NULL), the heavy work runs on the
// caller's thread — this is acceptable only from the EARLY-tier init error
// path, which runs on the 8192-byte main task.
bb_err_t bb_mqtt_stop(bb_mqtt_t *handle_p)
{
    if (!handle_p || !*handle_p) return BB_OK;
    bb_mqtt_destroy(*handle_p);
    *handle_p = NULL;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// EARLY-tier self-registration
// ---------------------------------------------------------------------------

#if CONFIG_BB_MQTT_AUTOREGISTER

// Module-level handle so the autoregistered client lives for the app lifetime.
static bb_mqtt_t s_auto_client = NULL;

// Reentrancy guard: held for the full lifetime of any in-flight reconfigure
// worker task so rapid second calls coalesce (find mutex taken, return BB_OK).
static SemaphoreHandle_t s_reconfig_lock = NULL;

// ---------------------------------------------------------------------------
// One-shot reconfigure worker task
//
// Runs on a dedicated 8192-byte FreeRTOS task ("mqtt_reconf") so that the
// heavy esp-mqtt stop→destroy→init→start + mbedTLS chain NEVER executes on
// the caller's thread.  The httpd worker has only a 6144-byte stack; that is
// enough for the PATCH handler itself but not for the teardown+TLS path.
//
// Lifetime: s_reconfig_lock is taken by the CALLER before spawning the task,
// then given back by the task just before vTaskDelete(NULL).  This means any
// concurrent call finds the mutex taken and returns BB_OK immediately
// (coalesced) rather than spawning a second overlapping task.
// ---------------------------------------------------------------------------

static void mqtt_reconf_task(void *arg)
{
    (void)arg;

    // --- Tear down existing client -------------------------------------------
    // Cancel any pending deferred start that belongs to the client we are
    // about to destroy.  Clear s_pending_start only when it points to our
    // client, not to a future handle.
    if (s_pending_start != NULL &&
        s_pending_start == (bb_mqtt_handle_t *)s_auto_client) {
        s_pending_start = NULL;
    }

    if (s_auto_client) {
        bb_mqtt_destroy(s_auto_client);
        s_auto_client = NULL;
    }

    // --- Re-read config from NVS ---------------------------------------------
    char enabled_str[4] = "0";
    bb_nv_get_str(BB_MQTT_NVS_NS, "enabled", enabled_str, sizeof(enabled_str), "0");
    if (enabled_str[0] != '1') {
        bb_log_i(TAG, "mqtt reconfigured (uri=<none>, enabled=false)");
        xSemaphoreGive(s_reconfig_lock);
        vTaskDelete(NULL);
        return;  // unreachable but keeps static analysis happy
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
        bb_log_w(TAG, "reconfigure: uri not set");
        xSemaphoreGive(s_reconfig_lock);
        vTaskDelete(NULL);
        return;
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
        bb_log_w(TAG, "reconfigure: bb_mqtt_init failed: %d", rc);
    } else {
        bb_log_i(TAG, "mqtt reconfigured (uri=%s, enabled=true)", uri);
    }

    xSemaphoreGive(s_reconfig_lock);
    vTaskDelete(NULL);
}

static bb_err_t bb_mqtt_autoregister_init(void)
{
    // Create reentrancy lock on first init.
    if (!s_reconfig_lock) {
        s_reconfig_lock = xSemaphoreCreateMutex();
    }

    // Check enabled flag in NVS.
    char enabled_str[4] = "0";
    bb_nv_get_str(BB_MQTT_NVS_NS, "enabled", enabled_str, sizeof(enabled_str), "0");
    if (enabled_str[0] != '1') {
        bb_log_d(TAG, "autoregister: disabled via NVS");
        return BB_OK;
    }

    char uri[BB_MQTT_URI_MAX]       = {0};
    char client_id[BB_MQTT_CLIENT_ID_MAX] = {0};
    char username[BB_MQTT_USER_MAX] = {0};
    char password[BB_MQTT_PASS_MAX] = {0};
    char tls_str[4]                 = "0";

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
        // client_id: empty string stored in NVS means "use hostname" (NULL)
        // a literal empty means "broker-assigned" — distinguish via tls key
        // For autoregister: treat empty client_id as NULL (use hostname).
        .client_id = client_id[0] ? client_id : NULL,
        .username  = username[0]  ? username  : NULL,
        .password  = password[0]  ? password  : NULL,
        .tls       = (tls_str[0] == '1'),
        .creds_ns  = BB_MQTT_NVS_NS,
    };

    // Boot init runs on the 8192-byte main task — safe to call directly.
    bb_err_t rc = bb_mqtt_init(&cfg, &s_auto_client);
    if (rc != BB_OK) {
        bb_log_w(TAG, "autoregister init failed: %d", rc);
    }
    return BB_OK;  // non-fatal: EARLY walk continues
}

BB_REGISTRY_REGISTER_EARLY(bb_mqtt, bb_mqtt_autoregister_init);

bb_err_t bb_mqtt_reconfigure(void)
{
    // Guard: autoregister must have run (lock created).
    if (!s_reconfig_lock) {
        bb_log_w(TAG, "reconfigure: not yet initialised");
        return BB_ERR_INVALID_STATE;
    }

    // Reentrancy guard — only one reconfigure task at a time.
    // Non-blocking try: if a task is already in flight, coalesce (return BB_OK).
    // The in-flight task will pick up the latest NVS config when it re-reads.
    if (xSemaphoreTake(s_reconfig_lock, 0) != pdTRUE) {
        bb_log_w(TAG, "reconfigure: already in progress, coalescing");
        return BB_OK;
    }

    // Spawn the one-shot worker.  The task holds s_reconfig_lock and gives
    // it back just before vTaskDelete(NULL).  Stack 8192 bytes to handle
    // esp-mqtt teardown + mbedTLS init; priority 5 (below httpd default of 5,
    // but httpd is already returned to its pool so there is no contention).
    BaseType_t ok = xTaskCreate(mqtt_reconf_task, "mqtt_reconf",
                                8192 / sizeof(StackType_t), NULL, 5, NULL);
    if (ok != pdPASS) {
        bb_log_e(TAG, "reconfigure: xTaskCreate failed");
        xSemaphoreGive(s_reconfig_lock);
        return BB_ERR_NO_SPACE;
    }

    bb_log_i(TAG, "reconfigure: worker task spawned");
    return BB_OK;  // caller (PATCH handler) returns 204 immediately
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
// bb_mqtt_reconfigure — stub when autoregister is disabled
// ---------------------------------------------------------------------------

#if !CONFIG_BB_MQTT_AUTOREGISTER
bb_err_t bb_mqtt_reconfigure(void)
{
    // Autoregister disabled; no managed client to reconfigure.
    return BB_ERR_INVALID_STATE;
}
#endif
