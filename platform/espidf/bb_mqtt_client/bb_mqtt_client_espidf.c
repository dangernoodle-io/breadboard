// bb_mqtt_client ESP-IDF backend — wraps esp-mqtt with TLS via bb_tls_creds.
//
// TLS lifetime: bb_tls_creds_t is allocated in the handle and freed only
// inside bb_mqtt_client_destroy, AFTER esp_mqtt_client_destroy completes.
// esp-mqtt does NOT copy certificate/key buffers, so the pointers must
// remain valid for the entire client lifetime.
//
// EARLY-tier self-registration: reads NVS "bb_mqtt" namespace for
// uri/client_id/username/password/enabled and connects when enabled=1.
// The handle lives for the app lifetime (register-on-enable, B1-289).
//
// Deferred start: esp_mqtt_client_start() is NOT called until the station
// has an IP address.  bb_mqtt_client_init observes the "wifi" bb_lifecycle
// service (B1-1045, replacing the old wifi.net bb_event subscription) and
// starts on its first ->RUNNING transition (or starts immediately when
// bb_wifi_has_ip() is already true).  Only the FIRST start is deferred;
// esp-mqtt's built-in reconnect handles later drops.
//
// Event-handler safety: esp-mqtt dispatches events via the ESP event loop,
// which can hold a queued event after esp_mqtt_client_stop returns.  To
// prevent a stale event firing into a freed handle we set h->destroyed =
// true and NULL h->client BEFORE calling esp_mqtt_client_destroy; the
// event handler checks h->destroyed under h->lock and returns immediately.
// enabled/uri/client_id/username/password/tls round-trip through bb_config
// (typed layer over bb_storage) rather than bb_nv's generic KV forwarder
// (B1-756, bb_nv dissolution epic B1-708) — bb_config's STR encoding
// resolves to the SAME nvs_get_str/nvs_set_str calls bb_nv_get_str/set_str
// made (both are thin forwarders to bb_storage_nvs, see bb_storage_nvs.h),
// so the on-flash namespace/key/STR-typed format below is byte-compatible
// with what this component previously read/wrote via bb_nv.
#include "bb_mqtt_client.h"
#include "bb_tls.h"
#include "bb_tls_creds.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_config.h"
#include "bb_mqtt_client_nvs.h"
#include "bb_settings.h"
#include "bb_wifi.h"
#include "bb_lifecycle.h"
#include "bb_mqtt_client_reassemble.h"  // PRIV_INCLUDE_DIRS "src" (bb_mqtt_client component)
#include "bb_mqtt_client_health.h"      // PRIV_INCLUDE_DIRS "src" (B1-1040 per-instance health)

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <inttypes.h>

#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bb_mqtt_client";

// BB_MQTT_CLIENT_NVS_NS is owned by bb_mqtt_client (bb_mqtt_client_nvs.h), not bb_nv.
#define BB_MQTT_CLIENT_URI_MAX       128
#define BB_MQTT_CLIENT_ID_MAX 64
#define BB_MQTT_CLIENT_USER_MAX      64
#define BB_MQTT_CLIENT_PASS_MAX      64

// B1-487: per-handle subscription-filter tracking, so MQTT_EVENT_CONNECTED
// can re-issue every subscribe after a reconnect (a fresh broker session
// does not always preserve subscriptions across a dropped TCP connection).
#define BB_MQTT_CLIENT_SUB_MAX      4
#define BB_MQTT_CLIENT_SUB_TOPIC_MAX 128

typedef struct {
    char topic[BB_MQTT_CLIENT_SUB_TOPIC_MAX];
    int  qos;
} bb_mqtt_client_sub_entry_t;

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
                                        // bb_mqtt_client_suspend_default; cleared by resume
    uint32_t                 reconnect_count;  // incremented on each reconnect
    bb_mqtt_client_disc_t           disc_reason;      // classified disconnect reason (B1-362)
    bb_tls_fail_t            tls_fail;         // TLS handshake failure class (B1-362)
    int                      tls_error_code;   // raw mbedtls err (0 = none) (B1-362)
    int64_t                  last_ok_ms;       // B1-1040: stamped on MQTT_EVENT_CONNECTED
    uint32_t                 fail_count;       // B1-1040: bumped on every MQTT_EVENT_DISCONNECTED
                                                // (unconditional -- see bb_mqtt_client.h)
    char                     uri[256];             // copy of cfg->uri for diag
    bb_mqtt_client_sub_entry_t      subs[BB_MQTT_CLIENT_SUB_MAX]; // B1-487: tracked filters
    int                      sub_count;
    bb_mqtt_client_msg_cb           msg_cb;    // B1-487: per-handle receive callback
    void                    *msg_ctx;
    bb_mqtt_client_reasm_state_t    reasm;     // per-handle reassembly state; reasm.buf
                                         // lazily allocated on first on_message(cb!=NULL)
} bb_mqtt_client_handle_t;

// ---------------------------------------------------------------------------
// B1-487: inbound message reassembly. Reassembly state (rx buffer, cursor,
// callback+ctx) lives on the handle (bb_mqtt_client_handle_t.reasm / .msg_cb /
// .msg_ctx) — NOT in process-wide statics — so two independently-created
// handles (e.g. a telemetry client and a separate ingress client, both
// possible in this codebase) can never splice bytes across each other's
// messages. The actual fragment-accumulation state machine is the pure,
// host-testable bb_mqtt_client_reasm_step (components/bb_mqtt_client/src/bb_mqtt_client_reassemble.c)
// — this file supplies only the esp_mqtt_event_handle_t field extraction.
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_client_on_message(bb_mqtt_client_t handle, bb_mqtt_client_msg_cb cb, void *ctx)
{
    if (!handle) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)handle;

    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (cb && !h->reasm.buf) {
        // Lazy heap allocation (SPIRAM-preferred): boards that only publish
        // and never call bb_mqtt_client_on_message pay zero BSS/heap for this.
        h->reasm.buf = bb_calloc_prefer_spiram(1, CONFIG_BB_MQTT_CLIENT_RX_BUFFER_BYTES);
        if (!h->reasm.buf) {
            xSemaphoreGive(h->lock);
            bb_log_w(TAG, "on_message: rx buffer alloc failed (%u bytes)",
                     (unsigned)CONFIG_BB_MQTT_CLIENT_RX_BUFFER_BYTES);
            return BB_ERR_NO_SPACE;
        }
        h->reasm.buf_cap = CONFIG_BB_MQTT_CLIENT_RX_BUFFER_BYTES;
        bb_mqtt_client_reasm_reset(&h->reasm);
    }
    h->msg_cb  = cb;
    h->msg_ctx = ctx;
    xSemaphoreGive(h->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Deferred-start support
//
// Only one handle can be pending a deferred start at a time (the autoregistered
// client).  When bb_mqtt_client_init runs at EARLY tier WiFi has not yet acquired
// an IP, so we observe the "wifi" bb_lifecycle service (B1-1045) and start on
// its first ->RUNNING transition.  If WiFi already has an IP at init time
// (e.g. a later manual call), we start immediately and skip the observer.
// ---------------------------------------------------------------------------

static _Atomic(bb_mqtt_client_handle_t *) s_pending_start = NULL;  // handle waiting for got-IP
// "wifi" bb_lifecycle service handle + one-time observer-registration guard.
// Both written exactly once from the single-threaded early-init composition
// root (bb_mqtt_client_init) and never mutated from mqtt_wifi_observer's
// dispatch context, so unlike s_pending_start above (read + cleared inside
// the observer) they need no atomic. Assumption: single-caller,
// single-thread-at-init -- the composition root calls bb_mqtt_client_init()
// exactly once from app_main, never concurrently from multiple tasks (see
// examples/floor/main/floor_app.c). If that ever changes, this guard needs
// a lock.
static bb_lifecycle_svc_t s_wifi_svc              = BB_LIFECYCLE_SVC_INVALID;
static bool               s_wifi_observer_attached = false;

static void mqtt_start_once(bb_mqtt_client_handle_t *h)
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
    bb_mqtt_client_handle_t *h = atomic_load(&s_pending_start);
    if (h) {
        atomic_store(&s_pending_start, NULL);  // clear before start; idempotent if called again
        mqtt_start_once(h);
    }
}

// "wifi" bb_lifecycle observer (B1-1045, replaces the old wifi.net bb_event
// subscriber). Acts only on the ->RUNNING edge (the GOT_IP-equivalent
// transition per bb_wifi_classify_lifecycle) -- mqtt never used the legacy
// on_disconnect hook, so a ->STOPPED/PAUSED transition is ignored here;
// esp-mqtt's built-in reconnect owns later drops.
static void mqtt_wifi_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)user;
    if (evt->svc != s_wifi_svc) return;
    if (evt->new_state == BB_LIFECYCLE_RUNNING && evt->old_state != BB_LIFECYCLE_RUNNING) {
        on_got_ip_cb();
    }
}

// ---------------------------------------------------------------------------
// URL host extraction helper (for TLS diagnostics)
// ---------------------------------------------------------------------------

// Extract hostname from a URI string into buf.  Returns buf on success or "?"
// when hostname cannot be determined.  Strips scheme (e.g. mqtts://), port,
// path, query, and fragment — leaves only the bare hostname.
static const char *mqtt_url_host(const char *url, char *buf, size_t buf_len)
{
    if (!url || !buf || buf_len == 0) return "?";
    const char *p = url;
    const char *s = strstr(p, "://");
    if (s) p = s + 3;
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (i + 1 < buf_len) buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return (i > 0) ? buf : "?";
}

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)arg;

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
    case MQTT_EVENT_CONNECTED: {
        // B1-487: snapshot tracked subs under the lock, then re-issue every
        // subscribe AFTER releasing the lock (esp_mqtt_client_subscribe must
        // not run while h->lock is held — the event handler itself takes
        // h->lock elsewhere, and esp-mqtt calls back into this handler from
        // the same task, so holding the lock here risks self-deadlock on a
        // reentrant dispatch).
        bb_mqtt_client_sub_entry_t local[BB_MQTT_CLIENT_SUB_MAX];
        int n;
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->connected = true;
        h->ever_connected = true;
        h->last_ok_ms = bb_mqtt_client_priv_now_ms64();  // B1-1040
        n = h->sub_count;
        memcpy(local, h->subs, sizeof(bb_mqtt_client_sub_entry_t) * (size_t)n);
        xSemaphoreGive(h->lock);
        bb_log_i(TAG, "connected");
        for (int i = 0; i < n; i++) {
            int msg_id = esp_mqtt_client_subscribe(h->client, local[i].topic, local[i].qos);
            if (msg_id < 0) {
                bb_log_w(TAG, "re-subscribe '%s' failed", local[i].topic);
            } else {
                bb_log_i(TAG, "re-subscribed '%s' (qos=%d)", local[i].topic, local[i].qos);
            }
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(h->lock, portMAX_DELAY);
        if (h->ever_connected) {
            h->reconnect_count++;
        }
        h->connected = false;
        // B1-1040: fail_count is unconditional -- unlike reconnect_count
        // above, it also captures a failed FIRST connect attempt (never
        // reached ever_connected). This event handler only ever sees a
        // DISCONNECTED that esp-mqtt itself dispatches for a network-level
        // drop -- a deliberate bb_mqtt_client_destroy() sets h->destroyed
        // BEFORE tearing the client down, and the guard at the top of this
        // handler returns early for any event dispatched after that point,
        // so a caller-initiated close never reaches here (see
        // bb_mqtt_client.h's reporting policy).
        h->fail_count++;
        xSemaphoreGive(h->lock);
        bb_log_i(TAG, "disconnected (reconnect_count=%" PRIu32 " fail_count=%" PRIu32 ")",
                 h->reconnect_count, h->fail_count);
        break;
    case MQTT_EVENT_DATA: {
        // B1-487: reassemble fragmented payload via the shared pure state
        // machine, then invoke the per-handle callback OUTSIDE the lock —
        // cb may call back into bb_mqtt_client_publish/subscribe on this same
        // handle, which would deadlock on a non-recursive mutex if we held
        // h->lock across the call. h->reasm.buf/topic are exclusively
        // written by this event task (esp-mqtt dispatches serially), so
        // reading them here after unlocking is safe.
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        bb_mqtt_client_msg_cb local_cb  = NULL;
        void          *local_ctx = NULL;
        size_t         local_len = 0;

        xSemaphoreTake(h->lock, portMAX_DELAY);
        if (h->msg_cb && h->reasm.buf) {
            bool complete = bb_mqtt_client_reasm_step(
                &h->reasm,
                event->topic, (size_t)event->topic_len,
                (size_t)event->total_data_len, (size_t)event->current_data_offset,
                event->data, (size_t)event->data_len);
            if (complete) {
                local_cb  = h->msg_cb;
                local_ctx = h->msg_ctx;
                local_len = h->reasm.len;
            }
        }
        xSemaphoreGive(h->lock);

        if (local_cb) {
            local_cb(h->reasm.topic, h->reasm.buf, local_len, local_ctx);
        }
        break;
    }
    case MQTT_EVENT_ERROR: {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        int tls_err = 0;
        xSemaphoreTake(h->lock, portMAX_DELAY);
        if (event->error_handle) {
            // Classify disconnect reason (B1-362)
            switch (event->error_handle->error_type) {
            case MQTT_ERROR_TYPE_NONE:
                h->disc_reason = BB_MQTT_CLIENT_DISC_NONE;
                break;
            case MQTT_ERROR_TYPE_TCP_TRANSPORT:
                h->disc_reason = BB_MQTT_CLIENT_DISC_TRANSPORT;
                if (event->error_handle->esp_tls_stack_err != 0) {
                    tls_err = event->error_handle->esp_tls_stack_err;
                }
                break;
            case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
                h->disc_reason = BB_MQTT_CLIENT_DISC_CONNECTION_REFUSED;
                break;
            default:
                h->disc_reason = BB_MQTT_CLIENT_DISC_OTHER;
                break;
            }
            h->tls_error_code = tls_err;
            h->tls_fail       = bb_tls_classify(tls_err);
        }
        xSemaphoreGive(h->lock);
        bb_log_w(TAG, "mqtt error event");
        if (tls_err != 0) {
            char diag_buf[256];
            char host_buf[128];
            const char *host = mqtt_url_host(h->uri, host_buf, sizeof(host_buf));
            bb_tls_handshake_diag(tls_err, host, BB_TLS_SSL_IN_LEN,
                                   diag_buf, sizeof(diag_buf));
            bb_log_w(TAG, "%s", diag_buf);
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_client_init(const bb_mqtt_client_cfg_t *cfg, bb_mqtt_client_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (!cfg->uri || !cfg->uri[0]) return BB_ERR_INVALID_ARG;

    bb_mqtt_client_handle_t *h = bb_calloc_prefer_spiram(1, sizeof(*h));
    if (!h) return BB_ERR_NO_SPACE;

    h->lock = xSemaphoreCreateMutex();
    if (!h->lock) {
        bb_mem_free(h);
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
    char hostname[33] = {0};
    bb_settings_hostname_get(hostname, sizeof(hostname), NULL);
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
            .keepalive = CONFIG_BB_MQTT_CLIENT_KEEPALIVE,
        },
        .network = {
            .timeout_ms = CONFIG_BB_MQTT_CLIENT_NETWORK_TIMEOUT_MS,
        },
        .buffer = {
            .size     = CONFIG_BB_MQTT_CLIENT_RX_BUFFER_BYTES,
            .out_size = CONFIG_BB_MQTT_CLIENT_TX_BUFFER_BYTES,
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
        bb_mem_free(h);
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
            bb_mem_free(h);
            return rc;
        }
        h->started = true;
    } else {
        // No IP yet — observe the "wifi" bb_lifecycle service, then start
        // immediately if WiFi acquired an IP in the narrow window between the
        // bb_wifi_has_ip() check above and the observer registration taking
        // effect. Register FIRST, then re-check has_ip -- bb_lifecycle_observe_async
        // has no replay of past transitions, so checking before registering
        // would leak a missed ->RUNNING edge. mqtt_start_once is idempotent
        // (h->started guard) so calling it from both paths is safe.
        atomic_store(&s_pending_start, h);
        if (!s_wifi_observer_attached) {
            if (bb_lifecycle_find("wifi", &s_wifi_svc) != BB_OK) {
                bb_log_w(TAG, "lifecycle_find(wifi) failed — mqtt start "
                              "will not fire on wifi ->RUNNING");
            } else if (bb_lifecycle_observe_async(mqtt_wifi_observer, NULL) != BB_OK) {
                bb_log_w(TAG, "lifecycle_observe_async(wifi) failed — mqtt start "
                              "will not fire on wifi ->RUNNING");
            }
            s_wifi_observer_attached = true;
        }
        if (bb_wifi_has_ip()) {
            on_got_ip_cb();
        }
        // If the immediate-fire path ran, h->started is already true and
        // we skip the deferred log to avoid the misleading message.
        if (!h->started) {
            bb_log_i(TAG, "init: start deferred until got-ip");
        }
    }

    h->tls = cfg->tls;
    snprintf(h->uri, sizeof(h->uri), "%s", cfg->uri ? cfg->uri : "");
    bb_log_i(TAG, "init: uri=%s tls=%d", cfg->uri, cfg->tls);
    *out = h;
    return BB_OK;
}

// B1-296 destroy-safety guard, shared by publish and subscribe: take h->lock,
// check destroyed/client under it, and capture the client pointer locally
// BEFORE releasing the lock so a concurrent bb_mqtt_client_destroy can't free it
// mid-use. Returns true (and writes *client_out, non-NULL) when the handle is
// live; false when destroyed or client is NULL. Caller must release the lock-
// free local immediately — never hold h->lock across a blocking esp-mqtt call
// (the event handler also takes h->lock).
static inline bool mqtt_acquire_client(bb_mqtt_client_handle_t *h,
                                       esp_mqtt_client_handle_t *client_out)
{
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool dead = h->destroyed || (h->client == NULL);
    *client_out = h->client;
    xSemaphoreGive(h->lock);
    return !dead;
}

bb_err_t bb_mqtt_client_publish(bb_mqtt_client_t handle, const char *topic,
                          const char *payload, int len, int qos, bool retain)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)handle;

    // Defense-in-depth (B1-296): take the lock, check destroyed/client under it,
    // capture the client pointer locally, then release BEFORE the blocking publish
    // call.  Holding the lock across esp_mqtt_client_publish would deadlock the
    // event handler (which also acquires h->lock on MQTT events).
    esp_mqtt_client_handle_t client;
    if (!mqtt_acquire_client(h, &client)) {
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

bb_err_t bb_mqtt_client_subscribe(bb_mqtt_client_t handle, const char *topic, int qos)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)handle;

    // Mirror the publish guard (B1-296): take the lock, check destroyed/client
    // under it, capture a local pointer, then release BEFORE the blocking call.
    // Holding the lock across esp_mqtt_client_subscribe would deadlock the
    // event handler (which also acquires h->lock on MQTT events).
    esp_mqtt_client_handle_t client;
    if (!mqtt_acquire_client(h, &client)) {
        bb_log_w(TAG, "subscribe skipped: handle destroyed or client NULL");
        return BB_ERR_INVALID_STATE;
    }

    // B1-487: track the filter so MQTT_EVENT_CONNECTED can re-subscribe it
    // on every (re)connect. Update qos in-place if the filter is already
    // tracked; otherwise append if there is room. Compare against the
    // truncated form (mirrors what would actually be stored) rather than a
    // raw strncmp bound to sizeof(topic) — a bare strncmp of the incoming
    // (possibly longer) topic against an already-truncated stored entry is
    // not a well-defined equality check for topics >= BB_MQTT_CLIENT_SUB_TOPIC_MAX.
    char topic_trunc[BB_MQTT_CLIENT_SUB_TOPIC_MAX];
    snprintf(topic_trunc, sizeof(topic_trunc), "%s", topic);

    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool tracked = false;
    for (int i = 0; i < h->sub_count; i++) {
        if (strcmp(h->subs[i].topic, topic_trunc) == 0) {
            h->subs[i].qos = qos;
            tracked = true;
            break;
        }
    }
    if (!tracked) {
        if (h->sub_count < BB_MQTT_CLIENT_SUB_MAX) {
            snprintf(h->subs[h->sub_count].topic, sizeof(h->subs[h->sub_count].topic), "%s", topic_trunc);
            h->subs[h->sub_count].qos = qos;
            h->sub_count++;
        } else {
            bb_log_w(TAG, "subscribe: filter registry full (max %d); '%s' will not be "
                          "re-subscribed on reconnect", BB_MQTT_CLIENT_SUB_MAX, topic);
        }
    }
    xSemaphoreGive(h->lock);

    int msg_id = esp_mqtt_client_subscribe(client, topic, qos);
    return (msg_id < 0) ? BB_ERR_INVALID_STATE : BB_OK;
}

bool bb_mqtt_client_is_connected(bb_mqtt_client_t handle)
{
    if (!handle) return false;
    bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)handle;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool c = h->connected;
    xSemaphoreGive(h->lock);
    return c;
}

bb_err_t bb_mqtt_client_get_stats(bb_mqtt_client_t handle, bb_mqtt_client_stats_t *out)
{
    if (!handle || !out) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)handle;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    out->reconnect_count = h->reconnect_count;
    out->connected       = h->connected;
    out->disc_reason     = h->disc_reason;
    out->tls_fail        = h->tls_fail;
    out->tls_error_code  = h->tls_error_code;
    xSemaphoreGive(h->lock);
    return BB_OK;
}

bool bb_mqtt_client_is_tls(bb_mqtt_client_t handle)
{
    if (!handle) return false;
    return ((bb_mqtt_client_handle_t *)handle)->tls;
}

// B1-1040: reuses h->lock (already guards get_stats()'s cross-task read
// above) rather than introducing a second lock -- see bb_mqtt_client_health.c.
bb_err_t bb_mqtt_client_health_fill(bb_mqtt_client_t handle, bb_mqtt_client_health_snap_t *out)
{
    if (!handle || !out) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)handle;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    out->connected      = h->connected;
    out->last_ok_ms     = h->last_ok_ms;
    out->fail_count     = (uint64_t)h->fail_count;
    out->tls_error_code = (int64_t)h->tls_error_code;
    xSemaphoreGive(h->lock);
    return BB_OK;
}

bb_err_t bb_mqtt_client_destroy(bb_mqtt_client_t handle)
{
    if (!handle) return BB_OK;
    bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)handle;
    if (h->client) {
        // Mark destroyed under lock BEFORE stop/destroy so that any event
        // already queued in the event loop (and not yet dispatched when stop
        // returns) finds the guard and returns immediately instead of
        // dereferencing freed memory → InstrFetchProhibited.
        if (h->lock) {
            xSemaphoreTake(h->lock, portMAX_DELAY);
            h->destroyed = true;
            // B1-1040: a caller-initiated destroy is a clean close, not a
            // transport failure -- the shared helper clears connected
            // without touching fail_count (see bb_mqtt_client_health.h).
            bb_mqtt_client_priv_health_close(&h->connected);
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
    if (h->reasm.buf) {
        bb_mem_free(h->reasm.buf);
        h->reasm.buf = NULL;
    }
    if (h->lock) {
        vSemaphoreDelete(h->lock);
    }
    bb_mem_free(h);
    return BB_OK;
}

// bb_mqtt_client_stop — stop and destroy the client, clearing the handle.
// Idempotent: safe to call with a NULL or already-destroyed handle.
// Used by telemetry sink teardown (B1-275) to release the client when
// the exclusive sink slot is lost.
//
// SYNC on ESP-IDF (httpd-safe): stop+destroy do NOT run TLS/mbedTLS and
// have a combined stack depth well under 6144 bytes, so they are safe to
// call inline on the httpd thread.  The handle is NULLed before bb_mqtt_client_destroy
// is called so subsequent calls are idempotent.  This mirrors the inline
// disable path in bb_mqtt_client_reconfigure (B1-276).
bb_err_t bb_mqtt_client_stop(bb_mqtt_client_t *handle_p)
{
    if (!handle_p || !*handle_p) return BB_OK;
    bb_mqtt_client_destroy(*handle_p);
    *handle_p = NULL;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// EARLY-tier self-registration (B1-289: register-on-enable at boot)
// ---------------------------------------------------------------------------

#if CONFIG_BB_MQTT_CLIENT_AUTOREGISTER

// Module-level handle so the autoregistered client lives for the app lifetime.
//
// HANDLE STABILITY: s_auto_client is a bb_mqtt_client_t (void *) whose VALUE changes
// across suspend/resume — suspend destroys the heap-allocated handle object
// (frees task + buffers + struct, ~11 KB) and resume recreates it at a NEW
// address.  Any sink that captured the old pointer value holds a use-after-free
// reference after suspend (B1-296).
//
// Callers SHOULD resolve bb_mqtt_client_default() fresh on every publish
// call rather than caching the pointer at boot.  This means:
//
//  - During the suspend window (s_auto_client == NULL), bb_mqtt_client_publish()
//    receives NULL and returns BB_ERR_INVALID_ARG — a clean no-op.
//  - After resume, a caller resolving the handle fresh each time
//    automatically targets the new handle; no re-registration is needed.
//
// bb_mqtt_client_default() returns s_auto_client (NULL when destroyed, non-NULL when
// live).  Callers that snapshot the return value must treat NULL as "suspended".
static bb_mqtt_client_t s_auto_client = NULL;

// Namespace/keys owned by bb_mqtt_client (bb_mqtt_client_nvs.h): NVS_NS
// ("bb_mqtt")/"enabled"/"uri"/BB_MQTT_CLIENT_NVS_KEY_CLIENT_ID("client_id")/
// "username"/"password"/"tls" — all STR-typed decimal-flag or plain-string
// encodings, unchanged — do not change without a migration plan, this
// strands provisioned-board MQTT config otherwise.
static const bb_config_field_t s_mqtt_enabled_field = {
    .id          = "mqtt.enabled",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_MQTT_CLIENT_NVS_NS, .key = "enabled" },
    .max_len     = 4,
    .def         = { .str = "0" },
    .has_default = true,
};

static const bb_config_field_t s_mqtt_uri_field = {
    .id          = "mqtt.uri",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_MQTT_CLIENT_NVS_NS, .key = "uri" },
    .max_len     = BB_MQTT_CLIENT_URI_MAX,
    .def         = { .str = "" },
    .has_default = true,
};

static const bb_config_field_t s_mqtt_client_id_field = {
    .id          = "mqtt.client_id",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_MQTT_CLIENT_NVS_NS, .key = BB_MQTT_CLIENT_NVS_KEY_CLIENT_ID },
    .max_len     = BB_MQTT_CLIENT_ID_MAX,
    .def         = { .str = "" },
    .has_default = true,
};

static const bb_config_field_t s_mqtt_username_field = {
    .id          = "mqtt.username",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_MQTT_CLIENT_NVS_NS, .key = "username" },
    .max_len     = BB_MQTT_CLIENT_USER_MAX,
    .def         = { .str = "" },
    .has_default = true,
};

static const bb_config_field_t s_mqtt_password_field = {
    .id          = "mqtt.password",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_MQTT_CLIENT_NVS_NS, .key = "password" },
    .max_len     = BB_MQTT_CLIENT_PASS_MAX,
    .def         = { .str = "" },
    .has_default = true,
    .secret      = true,
};

static const bb_config_field_t s_mqtt_tls_field = {
    .id          = "mqtt.tls",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_MQTT_CLIENT_NVS_NS, .key = "tls" },
    .max_len     = 4,
    .def         = { .str = "0" },
    .has_default = true,
};

// auto_client_create_from_nvs — read NVS config and create the auto-client.
//
// Shared by bb_mqtt_client_init_default (boot) and bb_mqtt_client_resume_default (post-
// suspend recreate).  On success, s_auto_client is set to the new handle but
// NOT yet started; the caller is responsible for starting appropriately:
//  - Boot path:   deferred via got-IP callback (handled inside bb_mqtt_client_init).
//  - Resume path: immediate start (WiFi/IP already up post-boot).
//
// Returns BB_OK on success or when disabled/unconfigured (non-fatal).
// Returns an error code only when bb_mqtt_client_init itself fails.
static bb_err_t auto_client_create_from_nvs(void)
{
    size_t out_len = 0;

    char enabled_str[4] = "0";
    bb_config_get_str(&s_mqtt_enabled_field, enabled_str, sizeof(enabled_str), &out_len);
    if (enabled_str[0] != '1') {
        bb_log_d(TAG, "autoregister: disabled via NVS");
        return BB_OK;
    }

    char uri[BB_MQTT_CLIENT_URI_MAX]             = {0};
    char client_id[BB_MQTT_CLIENT_ID_MAX] = {0};
    char username[BB_MQTT_CLIENT_USER_MAX]       = {0};
    char password[BB_MQTT_CLIENT_PASS_MAX]       = {0};
    char tls_str[4]                       = "0";

    bb_config_get_str(&s_mqtt_uri_field,       uri,       sizeof(uri),       &out_len);
    bb_config_get_str(&s_mqtt_client_id_field, client_id, sizeof(client_id), &out_len);
    bb_config_get_str(&s_mqtt_username_field,  username,  sizeof(username),  &out_len);
    bb_config_get_str(&s_mqtt_password_field,  password,  sizeof(password),  &out_len);
    bb_config_get_str(&s_mqtt_tls_field,       tls_str,   sizeof(tls_str),   &out_len);

    if (!uri[0]) {
        bb_log_w(TAG, "autoregister: uri not set");
        return BB_OK;
    }

    bb_mqtt_client_cfg_t cfg = {
        .uri       = uri,
        .client_id = client_id[0] ? client_id : NULL,
        .username  = username[0]  ? username  : NULL,
        .password  = password[0]  ? password  : NULL,
        .tls       = (tls_str[0] == '1'),
        .creds_ns  = BB_MQTT_CLIENT_NVS_NS,
    };

    bb_err_t rc = bb_mqtt_client_init(&cfg, &s_auto_client);
    if (rc != BB_OK) {
        bb_log_w(TAG, "auto_client_create_from_nvs: bb_mqtt_client_init failed: %d", rc);
        s_auto_client = NULL;
    }
    return rc;
}

bb_err_t bb_mqtt_client_init_default(void)
{
    // Boot init runs on the 8192-byte main task — safe to call directly.
    // auto_client_create_from_nvs also sets up the deferred got-IP start inside
    // bb_mqtt_client_init when WiFi has no IP yet (the normal EARLY-tier path).
    bb_err_t rc = auto_client_create_from_nvs();
    if (rc != BB_OK) {
        bb_log_w(TAG, "autoregister init failed: %d", rc);
    }
    return BB_OK;  // non-fatal: EARLY walk continues
}

// bb_mqtt_client_stop_default — stop and destroy the auto-registered client.
// Called when MQTT loses the exclusive-sink slot at boot (B1-289: loser
// teardown without reboot).
// Idempotent: safe to call when s_auto_client is already NULL.
bb_err_t bb_mqtt_client_stop_default(void)
{
    bb_mqtt_client_handle_t *pending = atomic_load(&s_pending_start);
    if (pending != NULL && pending == (bb_mqtt_client_handle_t *)s_auto_client) {
        atomic_store(&s_pending_start, NULL);
    }
    return bb_mqtt_client_stop(&s_auto_client);
}

// bb_mqtt_client_suspend_default — quiesce the auto-client to free heap headroom for
// a heap-heavy TLS operation (e.g. a GitHub update-check handshake on a
// heap-tight ESP32-S2 board).
//
// Two modes controlled by CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY:
//
//   STOP-ONLY (CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY=y):
//     Calls esp_mqtt_client_stop() only.  Keeps the client handle, task, and
//     buffers resident (~11 KB).  Resume calls esp_mqtt_client_start() on the
//     same handle — no destroy, no realloc, no NVS reload.
//     s_auto_client remains NON-NULL during the suspend window.
//     ONLY safe when bb_mem_arena_tls already reserves enough headroom for the
//     TLS handshake without the full 11 KB free.
//
//   FULL RELEASE (CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY=n, default):
//     Calls bb_mqtt_client_stop(&s_auto_client) which stops + DESTROYS the client
//     and NULLs the handle.  Frees the entire ~11 KB budget.
//
// Contrast with bb_mqtt_client_stop_default() which is a permanent disable (never
// resumed).  suspend/resume is a transient bracket.
//
// CALLER CONTRACT: callers MUST NOT publish during the suspended window. In
// full-release mode s_auto_client is NULL so bb_mqtt_client_publish() returns
// BB_ERR_INVALID_ARG cleanly; in stop-only mode s_auto_client is non-NULL but
// the client is stopped so publish attempts queue in esp-mqtt's outbox (may
// be dropped on queue-full).
//
// Idempotent: no-op + BB_OK if already suspended.
static bool s_suspended = false;   // tracks whether we are in a suspend window

bb_err_t bb_mqtt_client_suspend_default(void)
{
    if (s_suspended) return BB_OK;   // idempotent

#if CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY
    // Stop-only: keep client resident, just halt the network activity.
    if (s_auto_client) {
        bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)s_auto_client;
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->suspended  = true;
        h->connected  = false;
        xSemaphoreGive(h->lock);
        esp_mqtt_client_stop(h->client);
        bb_log_i(TAG, "suspended (stop-only, client resident ~11KB)");
    }
    // s_auto_client intentionally left NON-NULL in stop-only mode.
#else
    // Full release: destroy + free everything (~11 KB freed).
    if (s_auto_client) {
        // Clear the pending-start pointer if it still refers to this handle,
        // so the got-IP callback does not fire into the freed handle.
        bb_mqtt_client_handle_t *pending = atomic_load(&s_pending_start);
        if (pending != NULL && pending == (bb_mqtt_client_handle_t *)s_auto_client) {
            atomic_store(&s_pending_start, NULL);
        }
        bb_mqtt_client_stop(&s_auto_client);  // destroys + NULLs s_auto_client
    }
    bb_log_i(TAG, "suspended (full release ~11KB for heap-heavy TLS op)");
#endif

    s_suspended = true;
    return BB_OK;
}

// bb_mqtt_client_resume_default — re-establish the auto-client after a
// bb_mqtt_client_suspend_default() call.
//
// Two modes controlled by CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY:
//
//   STOP-ONLY (CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY=y):
//     Calls esp_mqtt_client_start() on the resident handle.  No NVS read,
//     no realloc.  esp-mqtt reconnects from the existing config.
//
//   FULL RELEASE (CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY=n, default):
//     Calls auto_client_create_from_nvs() which re-initialises and restarts
//     the client from NVS-persisted configuration.
//
// Post-boot the station already has an IP address for the full-release path;
// if WiFi is down, bb_mqtt_client_init falls back to the deferred got-IP path.
//
// Idempotent: no-op + BB_OK if not suspended.
bb_err_t bb_mqtt_client_resume_default(void)
{
    if (!s_suspended) return BB_OK;   // idempotent

#if CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY
    // Stop-only: restart the resident client.
    if (s_auto_client) {
        bb_mqtt_client_handle_t *h = (bb_mqtt_client_handle_t *)s_auto_client;
        bb_err_t rc = esp_mqtt_client_start(h->client);
        if (rc != BB_OK) {
            bb_log_w(TAG, "resume (stop-only): esp_mqtt_client_start failed: %d", rc);
            // Do NOT clear s_suspended — let the caller retry.
            return rc;
        }
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->suspended = false;
        h->started   = true;
        xSemaphoreGive(h->lock);
        bb_log_i(TAG, "resumed (stop-only, same handle=%p)", s_auto_client);
    }
#else
    // Full release: recreate from NVS.
    bb_err_t rc = auto_client_create_from_nvs();
    if (rc != BB_OK) {
        bb_log_w(TAG, "resume: auto_client_create_from_nvs failed: %d", rc);
        // Do NOT clear s_suspended — let the caller retry.
        return rc;
    }
    bb_log_i(TAG, "resumed (client recreated from NVS, handle=%p)", s_auto_client);
#endif

    s_suspended = false;
    return BB_OK;
}

#endif /* CONFIG_BB_MQTT_CLIENT_AUTOREGISTER */

// ---------------------------------------------------------------------------
// Default handle accessor
// ---------------------------------------------------------------------------

bb_mqtt_client_t bb_mqtt_client_default(void)
{
#if CONFIG_BB_MQTT_CLIENT_AUTOREGISTER
    return s_auto_client;
#else
    return NULL;
#endif
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_stop_default — stub when autoregister is disabled
// ---------------------------------------------------------------------------

#if !CONFIG_BB_MQTT_CLIENT_AUTOREGISTER
bb_err_t bb_mqtt_client_stop_default(void)
{
    // Autoregister disabled; no managed client to stop.
    return BB_OK;
}

bb_err_t bb_mqtt_client_suspend_default(void)
{
    // Autoregister disabled; no managed client to suspend.
    return BB_OK;
}

bb_err_t bb_mqtt_client_resume_default(void)
{
    // Autoregister disabled; no managed client to resume.
    return BB_OK;
}
#endif
