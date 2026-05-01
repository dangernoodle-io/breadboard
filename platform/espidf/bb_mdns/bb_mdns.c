#include "bb_mdns.h"
#include "bb_mdns_lifecycle.h"
#include "bb_wifi.h"
#include "mdns.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "bb_hw.h"
#include "bb_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "bb_mdns";

// Browse subscription table
#define BB_MDNS_BROWSE_MAX 4
typedef struct {
    char service[32];
    char proto[8];
    bb_mdns_peer_cb on_peer;
    bb_mdns_peer_removed_cb on_removed;
    void *ctx;
    bool in_use;
} bb_mdns_browse_sub_t;
static bb_mdns_browse_sub_t s_subs[BB_MDNS_BROWSE_MAX];

// Dispatch worker infrastructure
#define BB_MDNS_EVT_QUEUE_DEPTH 16
static QueueHandle_t    s_evt_queue     = NULL;
static TaskHandle_t     s_dispatch_task = NULL;
static SemaphoreHandle_t s_subs_mutex     = NULL;
// Serializes lifecycle teardown across the wifi-disconnect callback and the
// esp_register_shutdown_handler path. Both can fire during esp_restart and the
// portable bb_mdns_lifecycle started→stopped transition is not atomic.
static SemaphoreHandle_t s_lifecycle_mutex = NULL;
static uint32_t         s_evt_drop_count = 0;

// Async TXT query infrastructure
#define BB_MDNS_QUERY_QUEUE_DEPTH 8

typedef struct {
    char instance_name[64];
    char service[32];
    char proto[8];
    uint32_t timeout_ms;
    bb_mdns_query_cb cb;
    void *ctx;
} bb_mdns_query_req_t;

static QueueHandle_t s_query_queue = NULL;
static TaskHandle_t  s_query_task  = NULL;

// Event struct: single contiguous alloc; key/value in txt[] point into payload[]
#define BB_MDNS_EVT_TXT_MAX 8
typedef struct {
    bool     is_removal;
    char     service[32];
    char     proto[8];
    char     instance_name[64];
    char     hostname[64];
    char     ip4[16];
    uint16_t port;
    size_t   txt_count;
    bb_mdns_txt_t txt[BB_MDNS_EVT_TXT_MAX];
    char     payload[256];   /* packed key\0value\0… */
} bb_mdns_evt_t;

// Event pool: static 4-slot pool to avoid per-event malloc
#define BB_MDNS_EVT_POOL_SIZE 4
static bb_mdns_evt_t s_evt_pool[BB_MDNS_EVT_POOL_SIZE];
static bool s_evt_in_use[BB_MDNS_EVT_POOL_SIZE];
static SemaphoreHandle_t s_evt_pool_lock = NULL;

// Forward declarations for pool functions (used by dispatch_task)
static bb_mdns_evt_t *evt_pool_alloc(void);
static void evt_pool_free(bb_mdns_evt_t *evt);

// App-injected mDNS hostname
static char s_mdns_hostname[64] = "bsp-device";
static bool s_mdns_hostname_set = false;

// Cached running hostname (set during init)
static char s_running_hostname[64] = {0};
static bool s_running_hostname_valid = false;

// App-injected mDNS service type and instance name
static char s_mdns_service_type[32] = "_bsp";
static bool s_mdns_service_type_set = false;
static char s_mdns_instance_name[64] = "BSP Device";
static bool s_mdns_instance_name_set = false;

// Lifecycle state machine
static bb_mdns_lifecycle_state_t s_lc = {0};

/* Coalescing re-announce timer: armed by bb_mdns_set_txt (post-start) and
 * bb_mdns_announce. Fires after BB_MDNS_ANNOUNCE_DELAY_US to emit a fresh
 * unsolicited mDNS announce — observers' IDF caches can miss TXT-only updates
 * without it. Restarting the timer on each set_txt call is intentional: the
 * last-write-wins coalesce delays the announce until the burst settles. */
#define BB_MDNS_ANNOUNCE_DELAY_US (100 * 1000)  /* 100 ms */
static esp_timer_handle_t s_announce_timer = NULL;

static int apply_announce_locked(void)
{
    /* Use WIFI_STA_DEF netif — same key used by bb_wifi_init_sta. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        bb_log_w(TAG, "announce: no STA netif");
        return -1;
    }
    esp_err_t err = mdns_netif_action(sta,
                                      MDNS_EVENT_ANNOUNCE_IP4 |
                                      MDNS_EVENT_ANNOUNCE_IP6);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_netif_action(ANNOUNCE) failed: %s", esp_err_to_name(err));
        return -1;
    }
    bb_log_d(TAG, "unsolicited re-announce sent");
    return 0;
}

static void announce_timer_cb(void *arg)
{
    (void)arg;
    if (!bb_mdns_lifecycle_is_started(&s_lc)) return;
    if (!s_lc.announce_dirty) return;
    s_lc.announce_dirty = false;
    apply_announce_locked();
}

static void announce_timer_ensure_created(void)
{
    if (s_announce_timer) return;
    esp_timer_create_args_t args = {
        .callback = announce_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bb_mdns_announce",
    };
    esp_err_t err = esp_timer_create(&args, &s_announce_timer);
    if (err != ESP_OK) {
        bb_log_w(TAG, "esp_timer_create(announce) failed: %s", esp_err_to_name(err));
    }
}

static void announce_arm(uint64_t delay_us)
{
    if (!s_announce_timer) return;
    bb_mdns_lifecycle_mark_dirty(&s_lc);
    /* esp_timer_start_once returns ESP_ERR_INVALID_STATE if already running;
     * restart to implement last-write-wins coalescing. */
    esp_timer_stop(s_announce_timer);  /* ignore error if not running */
    esp_err_t err = esp_timer_start_once(s_announce_timer, delay_us);
    if (err != ESP_OK) {
        bb_log_w(TAG, "announce timer arm failed: %s", esp_err_to_name(err));
    }
}

/* Pending TXT items: bb_mdns_set_txt() may be called before the service has
 * actually started (start happens on wifi got-ip, async). Buffer the kv pairs
 * and replay them in bb_mdns_start_internal so apps can configure TXT eagerly
 * without racing the wifi callback. */
#define BB_MDNS_TXT_PENDING_MAX 8
typedef struct { char key[16]; char value[64]; bool in_use; } bb_mdns_txt_pending_t;
static bb_mdns_txt_pending_t s_txt_pending[BB_MDNS_TXT_PENDING_MAX];

static void txt_pending_store(const char *key, const char *value)
{
    /* Update existing slot if key already pending; otherwise take first free. */
    int free_slot = -1;
    for (int i = 0; i < BB_MDNS_TXT_PENDING_MAX; i++) {
        if (s_txt_pending[i].in_use && strcmp(s_txt_pending[i].key, key) == 0) {
            strncpy(s_txt_pending[i].value, value, sizeof(s_txt_pending[i].value) - 1);
            s_txt_pending[i].value[sizeof(s_txt_pending[i].value) - 1] = '\0';
            return;
        }
        if (!s_txt_pending[i].in_use && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        bb_log_w(TAG, "txt pending buffer full, dropping %s=%s", key, value);
        return;
    }
    strncpy(s_txt_pending[free_slot].key, key, sizeof(s_txt_pending[free_slot].key) - 1);
    s_txt_pending[free_slot].key[sizeof(s_txt_pending[free_slot].key) - 1] = '\0';
    strncpy(s_txt_pending[free_slot].value, value, sizeof(s_txt_pending[free_slot].value) - 1);
    s_txt_pending[free_slot].value[sizeof(s_txt_pending[free_slot].value) - 1] = '\0';
    s_txt_pending[free_slot].in_use = true;
}

static void txt_pending_flush(const char *service_type)
{
    for (int i = 0; i < BB_MDNS_TXT_PENDING_MAX; i++) {
        if (!s_txt_pending[i].in_use) continue;
        esp_err_t err = mdns_service_txt_item_set(service_type, "_tcp",
                                                  s_txt_pending[i].key,
                                                  s_txt_pending[i].value);
        if (err != ESP_OK) {
            bb_log_w(TAG, "deferred txt %s=%s failed: %s",
                     s_txt_pending[i].key, s_txt_pending[i].value, esp_err_to_name(err));
        }
        s_txt_pending[i].in_use = false;
    }
}

// Find subscription by service and proto; return NULL if not found
/* Compare service/proto strings tolerantly: IDF passes the strings back to
 * the notifier with inconsistent leading-underscore handling depending on
 * the result path. Strip a leading '_' on both sides before comparing so
 * "_taipanminer" registered matches a result reporting "taipanminer" or
 * "_taipanminer". */
static int eq_token(const char *a, const char *b)
{
    if (!a || !b) return 0;
    if (a[0] == '_') a++;
    if (b[0] == '_') b++;
    return strcmp(a, b) == 0;
}

static bb_mdns_browse_sub_t *browse_sub_find(const char *service, const char *proto)
{
    for (int i = 0; i < BB_MDNS_BROWSE_MAX; i++) {
        if (s_subs[i].in_use &&
            eq_token(s_subs[i].service, service) &&
            eq_token(s_subs[i].proto, proto)) {
            return &s_subs[i];
        }
    }
    return NULL;
}

// Find unused slot; return NULL if table full
static bb_mdns_browse_sub_t *browse_sub_alloc(void)
{
    for (int i = 0; i < BB_MDNS_BROWSE_MAX; i++) {
        if (!s_subs[i].in_use) {
            return &s_subs[i];
        }
    }
    return NULL;
}

// Query worker: dequeues async TXT query requests, performs blocking
// mdns_query_txt on this task (not the caller's), fires callback.
static void bb_mdns_query_task(void *arg)
{
    (void)arg;
    bb_mdns_query_req_t req;
    for (;;) {
        if (xQueueReceive(s_query_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        mdns_result_t *results = NULL;
        esp_err_t err = mdns_query_txt(req.instance_name, req.service, req.proto,
                                       req.timeout_ms, &results);

        bb_mdns_query_result_t out = { .err = (err == ESP_OK) ? BB_OK : (bb_err_t)err };
        char ip4_buf[16] = "";
        bb_mdns_txt_t txt_view[8] = {0};

        if (err == ESP_OK && results) {
            if (results->addr) {
                for (mdns_ip_addr_t *a = results->addr; a; a = a->next) {
                    if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                        snprintf(ip4_buf, sizeof(ip4_buf), IPSTR,
                                 IP2STR(&a->addr.u_addr.ip4));
                        break;
                    }
                }
            }
            size_t n = results->txt_count > 8 ? 8 : results->txt_count;
            for (size_t i = 0; i < n; i++) {
                txt_view[i].key   = (char *)results->txt[i].key;
                txt_view[i].value = (char *)results->txt[i].value;
            }
            out.instance_name = results->instance_name;
            out.hostname      = results->hostname;
            out.ip4           = ip4_buf[0] ? ip4_buf : NULL;
            out.port          = results->port;
            out.txt           = n ? txt_view : NULL;
            out.txt_count     = n;
        }

        if (req.cb) req.cb(&out, req.ctx);

        if (results) mdns_query_results_free(results);
    }
}

// Dispatch task: dequeues events and fires consumer callbacks
static void bb_mdns_dispatch_task(void *arg)
{
    (void)arg;
    bb_mdns_evt_t *evt;
    for (;;) {
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) continue;
        bb_mdns_peer_cb on_peer = NULL;
        bb_mdns_peer_removed_cb on_removed = NULL;
        void *ctx = NULL;
        xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
        bb_mdns_browse_sub_t *sub = browse_sub_find(evt->service, evt->proto);
        if (sub) {
            on_peer    = sub->on_peer;
            on_removed = sub->on_removed;
            ctx        = sub->ctx;
        }
        xSemaphoreGive(s_subs_mutex);
        // If add event has no ip4, attempt a short A-record lookup before dispatch.
        // Blocks only the worker task (not the IDF mDNS task) — acceptable on LAN.
        if (!evt->is_removal && evt->ip4[0] == '\0' && evt->hostname[0] != '\0') {
            esp_ip4_addr_t out_ip;
            esp_err_t qerr = mdns_query_a(evt->hostname, 200, &out_ip);
            if (qerr == ESP_OK) {
                snprintf(evt->ip4, sizeof(evt->ip4), IPSTR, IP2STR(&out_ip));
            } else {
                bb_log_d(TAG, "A lookup for %s failed (%s), dispatching without ip4",
                         evt->hostname, esp_err_to_name(qerr));
            }
        }

        if (evt->is_removal) {
            if (on_removed) on_removed(evt->instance_name, ctx);
        } else if (on_peer) {
            bb_mdns_peer_t peer = {
                .instance_name = evt->instance_name,
                .hostname      = evt->hostname[0]   ? evt->hostname : NULL,
                .ip4           = evt->ip4[0]        ? evt->ip4      : NULL,
                .port          = evt->port,
                .txt           = evt->txt_count     ? evt->txt      : NULL,
                .txt_count     = evt->txt_count,
            };
            on_peer(&peer, ctx);
        }
        evt_pool_free(evt);
    }
}

// Allocate an event from the pool or return NULL if all slots busy
static bb_mdns_evt_t *evt_pool_alloc(void)
{
    if (!s_evt_pool_lock) return NULL;

    if (xSemaphoreTake(s_evt_pool_lock, portMAX_DELAY) != pdTRUE) return NULL;

    bb_mdns_evt_t *evt = NULL;
    for (int i = 0; i < BB_MDNS_EVT_POOL_SIZE; i++) {
        if (!s_evt_in_use[i]) {
            s_evt_in_use[i] = true;
            evt = &s_evt_pool[i];
            memset(evt, 0, sizeof(bb_mdns_evt_t));
            break;
        }
    }

    xSemaphoreGive(s_evt_pool_lock);
    return evt;
}

// Release an event back to the pool
static void evt_pool_free(bb_mdns_evt_t *evt)
{
    if (!evt || !s_evt_pool_lock) return;

    if (xSemaphoreTake(s_evt_pool_lock, portMAX_DELAY) != pdTRUE) return;

    for (int i = 0; i < BB_MDNS_EVT_POOL_SIZE; i++) {
        if (&s_evt_pool[i] == evt) {
            s_evt_in_use[i] = false;
            break;
        }
    }

    xSemaphoreGive(s_evt_pool_lock);
}

// Internal notifier called by mdns_browse for all results — runs on IDF mDNS task.
// Enqueues deep-copied events; does NOT touch s_subs[] directly.
static void internal_notifier(mdns_result_t *results)
{
    for (mdns_result_t *r = results; r; r = r->next) {
        bb_mdns_evt_t *evt = evt_pool_alloc();
        if (!evt) {
            bb_log_w(TAG, "notifier: event pool exhausted, dropping event");
            continue;
        }

        // Copy service/proto for dispatch-task lookup
        strncpy(evt->service, r->service_type ? r->service_type : "", sizeof(evt->service) - 1);
        strncpy(evt->proto,   r->proto        ? r->proto        : "", sizeof(evt->proto)   - 1);
        strncpy(evt->instance_name,
                r->instance_name ? r->instance_name : "",
                sizeof(evt->instance_name) - 1);

        if (r->ttl == 0) {
            evt->is_removal = true;
        } else {
            evt->is_removal = false;
            strncpy(evt->hostname, r->hostname ? r->hostname : "", sizeof(evt->hostname) - 1);
            evt->port = r->port;

            // First IPv4 address
            if (r->addr) {
                for (mdns_ip_addr_t *addr = r->addr; addr; addr = addr->next) {
                    if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                        snprintf(evt->ip4, sizeof(evt->ip4), IPSTR,
                                 IP2STR(&addr->addr.u_addr.ip4));
                        break;
                    }
                }
            }

            // TXT records: pack key\0value\0 into payload[], point txt[].key/value in
            size_t n = r->txt_count;
            if (n > BB_MDNS_EVT_TXT_MAX) {
                bb_log_w(TAG, "notifier: %zu txt records exceed cap %d, truncating",
                         n, BB_MDNS_EVT_TXT_MAX);
                n = BB_MDNS_EVT_TXT_MAX;
            }
            char *p = evt->payload;
            char *end = evt->payload + sizeof(evt->payload);
            for (size_t i = 0; i < n && p < end; i++) {
                const char *k = r->txt[i].key   ? r->txt[i].key   : "";
                const char *v = r->txt[i].value ? r->txt[i].value : "";
                size_t klen = strlen(k);
                size_t vlen = strlen(v);
                // Need k\0v\0 to fit
                if (p + klen + 1 + vlen + 1 > end) {
                    bb_log_w(TAG, "notifier: payload full, truncating txt at %zu", i);
                    break;
                }
                memcpy(p, k, klen + 1);
                evt->txt[i].key = p;
                p += klen + 1;
                memcpy(p, v, vlen + 1);
                evt->txt[i].value = p;
                p += vlen + 1;
                evt->txt_count++;
            }
        }

        if (xQueueSend(s_evt_queue, &evt, 0) != pdTRUE) {
            evt_pool_free(evt);
            s_evt_drop_count++;
            // Log once per 16 drops (suppress flood; bit 0 of (count-1) == 0 when count is 1, 17, 33…)
            if ((s_evt_drop_count & 0x0F) == 1) {
                bb_log_w(TAG, "evt queue full, %lu events dropped", (unsigned long)s_evt_drop_count);
            }
        }
    }
}

static void mdns_build_hostname(char *out, size_t out_size)
{
    if (s_mdns_hostname_set && s_mdns_hostname[0] != '\0') {
        snprintf(out, out_size, "%s", s_mdns_hostname);
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(out, out_size, "bsp-device-%02x%02x", mac[4], mac[5]);
    }

    /* mDNS label max 63 chars */
    out[63] = '\0';
}

static void mdns_build_instance_name(char *out, size_t out_size)
{
    const char *base = s_mdns_instance_name_set ? s_mdns_instance_name : "BSP Device";
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* Cap base at (out_size - 6) to leave room for "-xxxx\0".
       Without this, gcc's -Wformat-truncation proves the 64-byte
       s_mdns_instance_name could overflow a 64-byte out buffer. */
    const int base_max = (out_size > 6) ? (int)(out_size - 6) : 0;
    snprintf(out, out_size, "%.*s-%02x%02x", base_max, base, mac[4], mac[5]);
}

static int mdns_init_impl(void)
{
    char hostname[64];
    mdns_build_hostname(hostname, sizeof(hostname));

    // Cache the running hostname for bb_mdns_get_hostname()
    strncpy(s_running_hostname, hostname, sizeof(s_running_hostname) - 1);
    s_running_hostname[sizeof(s_running_hostname) - 1] = '\0';
    s_running_hostname_valid = true;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return -1;
    }
    char instance_name[64];
    mdns_build_instance_name(instance_name, sizeof(instance_name));
    err = mdns_instance_name_set(instance_name);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
        return -1;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    mdns_txt_item_t txt[] = {
        {"board",   BOARD_NAME},
        {"version", app->version},
        {"mac",     mac_str},
    };

    const char *service_type = s_mdns_service_type_set ? s_mdns_service_type : "_bsp";
    err = mdns_service_add(NULL, service_type, "_tcp", 80, txt, 3);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return -1;
    }

    txt_pending_flush(service_type);
    announce_timer_ensure_created();

    // Re-arm any existing browse subscriptions after reconnect (mutex held)
    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    for (int i = 0; i < BB_MDNS_BROWSE_MAX; i++) {
        if (!s_subs[i].in_use) continue;
        char svc[32], proto[8];
        strncpy(svc,   s_subs[i].service, sizeof(svc)   - 1); svc[sizeof(svc)-1]     = '\0';
        strncpy(proto, s_subs[i].proto,   sizeof(proto) - 1); proto[sizeof(proto)-1] = '\0';
        xSemaphoreGive(s_subs_mutex);
        mdns_browse_t *handle = mdns_browse_new(svc, proto, internal_notifier);
        if (!handle) {
            bb_log_w(TAG, "browse re-arm failed: %s.%s", svc, proto);
        } else {
            bb_log_d(TAG, "browse re-armed: %s.%s", svc, proto);
        }
        xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    }
    xSemaphoreGive(s_subs_mutex);

    bb_log_i(TAG, "mDNS started: %s.local (%s._tcp)", hostname, service_type);
    return 0;
}

static void mdns_free_impl(void)
{
    mdns_service_remove_all();
    mdns_free();
}

static void mdns_send_bye_impl(void)
{
    /* Give the mdns task time to emit bye packets before proceeding. */
    vTaskDelay(pdMS_TO_TICKS(100));
}

static const bb_mdns_lifecycle_adapter_t s_lc_adapter = {
    .mdns_init = mdns_init_impl,
    .mdns_free = mdns_free_impl,
    .mdns_apply_announce = apply_announce_locked,
    .mdns_send_bye = mdns_send_bye_impl,
};

static void bb_mdns_start_internal(void)
{
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_start(&s_lc, &s_lc_adapter);
    switch (res) {
    case BB_MDNS_LC_OK:
        break;
    case BB_MDNS_LC_ALREADY_STARTED:
        bb_log_d(TAG, "bb_mdns_start_internal: already started");
        break;
    case BB_MDNS_LC_INIT_FAILED:
        bb_log_e(TAG, "bb_mdns_start_internal: init failed");
        break;
    case BB_MDNS_LC_NOT_STARTED:
        bb_log_e(TAG, "bb_mdns_start_internal: invalid state");
        break;
    case BB_MDNS_LC_INVALID_ARG:
        bb_log_e(TAG, "bb_mdns_start_internal: invalid arg");
        break;
    }
}

// Callback invoked by bb_wifi when IP is obtained
static void bb_mdns_on_got_ip(void)
{
    bb_mdns_start_internal();
    if (bb_mdns_lifecycle_is_started(&s_lc)) {
        char instance_name[64];
        mdns_build_instance_name(instance_name, sizeof(instance_name));
        mdns_instance_name_set(instance_name);
    }
}

static void bb_mdns_on_disconnect(void)
{
    if (s_lifecycle_mutex) xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (!bb_mdns_lifecycle_is_started(&s_lc)) {
        if (s_lifecycle_mutex) xSemaphoreGive(s_lifecycle_mutex);
        return;
    }
    bb_log_i(TAG, "wifi disconnected — tearing down mdns");
    if (s_announce_timer) {
        esp_timer_stop(s_announce_timer);
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
    }

    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_stop(&s_lc, &s_lc_adapter);
    if (res != BB_MDNS_LC_OK && res != BB_MDNS_LC_NOT_STARTED) {
        bb_log_w(TAG, "bb_mdns_on_disconnect: lifecycle stop returned %d", res);
    }
    if (s_lifecycle_mutex) xSemaphoreGive(s_lifecycle_mutex);
}

static void bb_mdns_shutdown(void)
{
    if (s_lifecycle_mutex) xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (!bb_mdns_lifecycle_is_started(&s_lc)) {
        if (s_lifecycle_mutex) xSemaphoreGive(s_lifecycle_mutex);
        return;
    }
    if (s_announce_timer) {
        esp_timer_stop(s_announce_timer);
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
    }

    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_stop(&s_lc, &s_lc_adapter);
    if (res != BB_MDNS_LC_OK && res != BB_MDNS_LC_NOT_STARTED) {
        bb_log_w(TAG, "bb_mdns_shutdown: lifecycle stop returned %d", res);
    }
    if (s_lifecycle_mutex) xSemaphoreGive(s_lifecycle_mutex);
}

void bb_mdns_init(void)
{
    // Create mutex and dispatch infrastructure once (outlives WiFi cycles)
    if (!s_subs_mutex) {
        s_subs_mutex = xSemaphoreCreateMutex();
    }
    if (!s_lifecycle_mutex) {
        s_lifecycle_mutex = xSemaphoreCreateMutex();
    }
    if (!s_evt_pool_lock) {
        s_evt_pool_lock = xSemaphoreCreateMutex();
    }
    if (!s_evt_queue) {
        s_evt_queue = xQueueCreate(BB_MDNS_EVT_QUEUE_DEPTH, sizeof(bb_mdns_evt_t *));
    }
    if (!s_dispatch_task) {
        xTaskCreate(bb_mdns_dispatch_task, "bb_mdns_disp", 4096, NULL, 3, &s_dispatch_task);
    }
    if (!s_query_queue) {
        s_query_queue = xQueueCreate(BB_MDNS_QUERY_QUEUE_DEPTH, sizeof(bb_mdns_query_req_t));
    }
    if (!s_query_task) {
        xTaskCreate(bb_mdns_query_task, "bb_mdns_query", 4096, NULL, 3, &s_query_task);
    }

    // Register callback with bb_wifi
    bb_wifi_register_on_got_ip(bb_mdns_on_got_ip);
    bb_wifi_register_on_disconnect(bb_mdns_on_disconnect);
    esp_register_shutdown_handler(bb_mdns_shutdown);
}

void bb_mdns_set_hostname(const char *hostname)
{
    if (!hostname) {
        s_mdns_hostname[0] = '\0';
        s_mdns_hostname_set = false;
        return;
    }
    strncpy(s_mdns_hostname, hostname, sizeof(s_mdns_hostname) - 1);
    s_mdns_hostname[sizeof(s_mdns_hostname) - 1] = '\0';
    s_mdns_hostname_set = true;
}

void bb_mdns_set_service_type(const char *service_type)
{
    if (!service_type) {
        s_mdns_service_type[0] = '\0';
        s_mdns_service_type_set = false;
        return;
    }
    strncpy(s_mdns_service_type, service_type, sizeof(s_mdns_service_type) - 1);
    s_mdns_service_type[sizeof(s_mdns_service_type) - 1] = '\0';
    s_mdns_service_type_set = true;
}

void bb_mdns_set_instance_name(const char *instance_name)
{
    if (!instance_name) {
        s_mdns_instance_name[0] = '\0';
        s_mdns_instance_name_set = false;
        return;
    }
    strncpy(s_mdns_instance_name, instance_name, sizeof(s_mdns_instance_name) - 1);
    s_mdns_instance_name[sizeof(s_mdns_instance_name) - 1] = '\0';
    s_mdns_instance_name_set = true;
}

bool bb_mdns_started(void)
{
    return bb_mdns_lifecycle_is_started(&s_lc);
}

const char *bb_mdns_get_hostname(void)
{
    if (!s_running_hostname_valid || !bb_mdns_lifecycle_is_started(&s_lc)) {
        return NULL;
    }
    return s_running_hostname;
}

void bb_mdns_set_txt(const char *key, const char *value)
{
    if (!key || !value) return;
    if (!bb_mdns_lifecycle_is_started(&s_lc)) {
        /* Buffer until service starts (typically when wifi gets an IP). */
        txt_pending_store(key, value);
        return;
    }
    const char *service_type = s_mdns_service_type_set ? s_mdns_service_type : "_bsp";
    esp_err_t err = mdns_service_txt_item_set(service_type, "_tcp", key, value);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_service_txt_item_set(%s=%s) failed: %s",
                 key, value, esp_err_to_name(err));
    }
    /* Arm coalescing re-announce: observers may miss TXT-only updates without
     * an unsolicited announce. Restart timer on every set_txt so the announce
     * fires after the burst settles (last-write-wins, 100 ms window). */
    announce_arm(BB_MDNS_ANNOUNCE_DELAY_US);
}

void bb_mdns_announce(void)
{
    if (!bb_mdns_lifecycle_is_started(&s_lc)) {
        bb_log_d(TAG, "bb_mdns_announce: service not started, no-op");
        return;
    }
    /* Bypass coalescing delay for explicit calls — fire immediately. */
    announce_arm(0);
}

bb_err_t bb_mdns_browse_start(const char *service, const char *proto,
                              bb_mdns_peer_cb on_peer,
                              bb_mdns_peer_removed_cb on_removed,
                              void *ctx)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);

    // Check if already subscribed; if so, update callbacks (idempotent)
    bb_mdns_browse_sub_t *existing = browse_sub_find(service, proto);
    if (existing) {
        existing->on_peer    = on_peer;
        existing->on_removed = on_removed;
        existing->ctx        = ctx;
        xSemaphoreGive(s_subs_mutex);
        return BB_OK;
    }

    // Not yet subscribed; find free slot
    bb_mdns_browse_sub_t *slot = browse_sub_alloc();
    if (!slot) {
        xSemaphoreGive(s_subs_mutex);
        return BB_ERR_NO_SPACE;
    }

    // Populate slot
    strncpy(slot->service, service, sizeof(slot->service) - 1);
    slot->service[sizeof(slot->service) - 1] = '\0';
    strncpy(slot->proto, proto, sizeof(slot->proto) - 1);
    slot->proto[sizeof(slot->proto) - 1] = '\0';
    slot->on_peer    = on_peer;
    slot->on_removed = on_removed;
    slot->ctx        = ctx;
    slot->in_use     = true;

    xSemaphoreGive(s_subs_mutex);

    // Start browse outside lock (can be slow)
    mdns_browse_t *handle = mdns_browse_new(service, proto, internal_notifier);
    if (!handle) {
        // Clear slot on failure
        xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
        memset(slot, 0, sizeof(*slot));
        xSemaphoreGive(s_subs_mutex);
        bb_log_e(TAG, "mdns_browse_new(%s.%s) failed", service, proto);
        return BB_ERR_INVALID_STATE;
    }

    bb_log_d(TAG, "browse started: %s.%s", service, proto);
    return BB_OK;
}

bb_err_t bb_mdns_browse_stop(const char *service, const char *proto)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    bb_mdns_browse_sub_t *sub = browse_sub_find(service, proto);
    if (!sub) {
        xSemaphoreGive(s_subs_mutex);
        return BB_OK;  // Already unstarted (idempotent)
    }

    // Clear slot under lock
    memset(sub, 0, sizeof(*sub));
    xSemaphoreGive(s_subs_mutex);

    // Stop browse outside lock
    esp_err_t err = mdns_browse_delete(service, proto);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_browse_delete(%s.%s) failed: %s",
                 service, proto, esp_err_to_name(err));
    }

    bb_log_d(TAG, "browse stopped: %s.%s", service, proto);
    return BB_OK;
}

bb_err_t bb_mdns_query_txt(const char *instance_name, const char *service, const char *proto,
                           uint32_t timeout_ms, bb_mdns_query_cb cb, void *ctx)
{
    if (!instance_name || !service || !proto) return BB_ERR_INVALID_ARG;
    if (!s_query_queue) return BB_ERR_INVALID_STATE;
    bb_mdns_query_req_t req = { .timeout_ms = timeout_ms, .cb = cb, .ctx = ctx };
    strncpy(req.instance_name, instance_name, sizeof(req.instance_name) - 1);
    strncpy(req.service,       service,       sizeof(req.service)       - 1);
    strncpy(req.proto,         proto,         sizeof(req.proto)         - 1);
    if (xQueueSend(s_query_queue, &req, 0) != pdTRUE) return BB_ERR_NO_SPACE;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Registry auto-registration
// ---------------------------------------------------------------------------

#include "bb_registry.h"
#include "bb_http.h"

static bb_err_t bb_mdns_registry_init(bb_http_handle_t server)
{
    (void)server;
    bb_mdns_init();
    return BB_OK;
}

#if CONFIG_BB_MDNS_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_mdns, bb_mdns_registry_init, 0);
#endif
