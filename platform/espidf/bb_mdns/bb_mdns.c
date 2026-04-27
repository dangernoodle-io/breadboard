#include "bb_mdns.h"
#include "bb_wifi.h"
#include "mdns.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bb_hw.h"
#include "bb_log.h"
#include <string.h>

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

// App-injected mDNS hostname
static char s_mdns_hostname[64] = "bsp-device";
static bool s_mdns_hostname_set = false;

// App-injected mDNS service type and instance name
static char s_mdns_service_type[32] = "_bsp";
static bool s_mdns_service_type_set = false;
static char s_mdns_instance_name[64] = "BSP Device";
static bool s_mdns_instance_name_set = false;

static bool s_mdns_started = false;

/* Coalescing re-announce timer: armed by bb_mdns_set_txt (post-start) and
 * bb_mdns_announce. Fires after BB_MDNS_ANNOUNCE_DELAY_US to emit a fresh
 * unsolicited mDNS announce — observers' IDF caches can miss TXT-only updates
 * without it. Restarting the timer on each set_txt call is intentional: the
 * last-write-wins coalesce delays the announce until the burst settles. */
#define BB_MDNS_ANNOUNCE_DELAY_US (100 * 1000)  /* 100 ms */
static esp_timer_handle_t s_announce_timer = NULL;
static volatile bool s_announce_dirty = false;

static void announce_timer_cb(void *arg)
{
    (void)arg;
    if (!s_announce_dirty || !s_mdns_started) return;
    s_announce_dirty = false;
    /* Use WIFI_STA_DEF netif — same key used by bb_wifi_init_sta. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        bb_log_w(TAG, "announce: no STA netif");
        return;
    }
    esp_err_t err = mdns_netif_action(sta,
                                      MDNS_EVENT_ANNOUNCE_IP4 |
                                      MDNS_EVENT_ANNOUNCE_IP6);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_netif_action(ANNOUNCE) failed: %s", esp_err_to_name(err));
    } else {
        bb_log_d(TAG, "unsolicited re-announce sent");
    }
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
    s_announce_dirty = true;
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

// Internal notifier called by mdns_browse for all results
static void internal_notifier(mdns_result_t *results)
{
    for (mdns_result_t *r = results; r; r = r->next) {
        // Find matching subscription. IDF's mdns_browse delivers results
        // already filtered to the requested service, but the result's
        // service_type/proto strings may or may not include leading '_';
        // eq_token normalizes that.
        bb_mdns_browse_sub_t *sub = browse_sub_find(r->service_type, r->proto);
        if (!sub) {
            bb_log_d(TAG, "no sub for result %s.%s",
                     r->service_type ? r->service_type : "(null)",
                     r->proto ? r->proto : "(null)");
            continue;
        }

        // Check if removal (ttl == 0)
        if (r->ttl == 0) {
            if (sub->on_removed) {
                sub->on_removed(r->instance_name, sub->ctx);
            }
            continue;
        }

        // Build peer structure with stack-local allocations
        char ip4_buf[16] = "";
        const char *ip4 = NULL;

        // Find first IPv4 address
        if (r->addr) {
            mdns_ip_addr_t *addr = r->addr;
            for (; addr; addr = addr->next) {
                if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                    snprintf(ip4_buf, sizeof(ip4_buf), IPSTR, IP2STR(&addr->addr.u_addr.ip4));
                    ip4 = ip4_buf;
                    break;
                }
            }
        }

        // Build TXT array (point to IDF's arrays; they're valid for the call)
        bb_mdns_txt_t txt_view[r->txt_count];
        for (size_t i = 0; i < r->txt_count; i++) {
            txt_view[i].key = (char *)r->txt[i].key;
            txt_view[i].value = (char *)r->txt[i].value;
        }

        // Build peer and invoke callback
        bb_mdns_peer_t peer = {
            .instance_name = r->instance_name,
            .hostname = r->hostname,
            .ip4 = ip4,
            .port = r->port,
            .txt = txt_view,
            .txt_count = r->txt_count,
        };

        if (sub->on_peer) {
            sub->on_peer(&peer, sub->ctx);
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

static void bb_mdns_start_internal(void)
{
    if (s_mdns_started) {
        return;
    }

    char hostname[64];
    mdns_build_hostname(hostname, sizeof(hostname));

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return;
    }
    char instance_name[64];
    mdns_build_instance_name(instance_name, sizeof(instance_name));
    err = mdns_instance_name_set(instance_name);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
        return;
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
        return;
    }
    s_mdns_started = true;
    txt_pending_flush(service_type);
    announce_timer_ensure_created();

    // Re-arm any existing browse subscriptions after reconnect
    for (int i = 0; i < BB_MDNS_BROWSE_MAX; i++) {
        if (!s_subs[i].in_use) continue;
        mdns_browse_t *handle = mdns_browse_new(s_subs[i].service, s_subs[i].proto, internal_notifier);
        if (!handle) {
            bb_log_w(TAG, "browse re-arm failed: %s.%s", s_subs[i].service, s_subs[i].proto);
        } else {
            bb_log_d(TAG, "browse re-armed: %s.%s", s_subs[i].service, s_subs[i].proto);
        }
    }

    bb_log_i(TAG, "mDNS started: %s.local (%s._tcp)", hostname, service_type);
}

// Callback invoked by bb_wifi when IP is obtained
static void bb_mdns_on_got_ip(void)
{
    bb_mdns_start_internal();
    if (s_mdns_started) {
        char instance_name[64];
        mdns_build_instance_name(instance_name, sizeof(instance_name));
        mdns_instance_name_set(instance_name);
    }
}

static void bb_mdns_on_disconnect(void)
{
    if (!s_mdns_started) return;
    bb_log_i(TAG, "wifi disconnected — tearing down mdns");
    if (s_announce_timer) {
        esp_timer_stop(s_announce_timer);
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
    }
    s_announce_dirty = false;
    mdns_service_remove_all();
    mdns_free();
    s_mdns_started = false;
}

static void bb_mdns_shutdown(void)
{
    if (!s_mdns_started) return;
    if (s_announce_timer) {
        esp_timer_stop(s_announce_timer);
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
    }
    s_announce_dirty = false;
    mdns_service_remove_all();
    // Give the mdns task time to emit bye packets before reboot
    vTaskDelay(pdMS_TO_TICKS(100));
    mdns_free();
}

void bb_mdns_init(void)
{
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
    return s_mdns_started;
}

void bb_mdns_set_txt(const char *key, const char *value)
{
    if (!key || !value) return;
    if (!s_mdns_started) {
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
    if (!s_mdns_started) {
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

    // Check if already subscribed; if so, update callbacks (idempotent)
    bb_mdns_browse_sub_t *existing = browse_sub_find(service, proto);
    if (existing) {
        existing->on_peer = on_peer;
        existing->on_removed = on_removed;
        existing->ctx = ctx;
        return BB_OK;
    }

    // Not yet subscribed; find free slot
    bb_mdns_browse_sub_t *slot = browse_sub_alloc();
    if (!slot) {
        return BB_ERR_NO_SPACE;
    }

    // Populate slot
    strncpy(slot->service, service, sizeof(slot->service) - 1);
    slot->service[sizeof(slot->service) - 1] = '\0';
    strncpy(slot->proto, proto, sizeof(slot->proto) - 1);
    slot->proto[sizeof(slot->proto) - 1] = '\0';
    slot->on_peer = on_peer;
    slot->on_removed = on_removed;
    slot->ctx = ctx;

    // Start browse (mdns_browse_new returns a handle, NULL on failure)
    mdns_browse_t *handle = mdns_browse_new(service, proto, internal_notifier);
    if (!handle) {
        // Clear slot on failure
        memset(slot, 0, sizeof(*slot));
        bb_log_e(TAG, "mdns_browse_new(%s.%s) failed", service, proto);
        return BB_ERR_INVALID_STATE;
    }

    slot->in_use = true;
    bb_log_d(TAG, "browse started: %s.%s", service, proto);
    return BB_OK;
}

bb_err_t bb_mdns_browse_stop(const char *service, const char *proto)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }

    bb_mdns_browse_sub_t *sub = browse_sub_find(service, proto);
    if (!sub) {
        return BB_OK;  // Already unstarted (idempotent)
    }

    // Stop browse
    esp_err_t err = mdns_browse_delete(service, proto);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_browse_delete(%s.%s) failed: %s",
                 service, proto, esp_err_to_name(err));
    }

    // Clear slot regardless of error
    memset(sub, 0, sizeof(*sub));
    bb_log_d(TAG, "browse stopped: %s.%s", service, proto);
    return BB_OK;
}
