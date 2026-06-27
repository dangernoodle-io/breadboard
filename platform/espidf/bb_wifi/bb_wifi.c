#include "bb_wifi.h"
#include "bb_nv.h"
#include "bb_nv_wifi_pending.h"
#include "wifi_reconn.h"
#include "bb_registry.h"
#include <string.h>
#include <stdatomic.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "bb_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "bb_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "bb_timer.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "bb_ota_validator.h"

static const char *TAG = "bb_wifi";
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_MAX_RETRY 10

// State tracking
static bool s_netif_initialized = false;
static esp_netif_t *s_sta_netif = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static atomic_int s_retry_count = 0;
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;
static bb_oneshot_timer_t s_reconnect_timer = NULL;
static volatile bool s_has_ip = false;
#if CONFIG_BB_WIFI_RECONFIGURE
static volatile bool s_pending_try = false;
#endif

// Set while bb_wifi_restart_sta() is in progress so event_handler skips the
// auto-connect on WIFI_EVENT_STA_START (the restart fn calls connect explicitly).
// Also suppresses the DISCONNECTED event that esp_wifi_stop() generates internally.
static volatile bool s_sta_restarting = false;

// Cached STA config — stored at wifi_connect_sta_ex time so bb_wifi_restart_sta()
// can re-apply it after stop/start without re-reading NVS.
static wifi_config_t s_sta_config;

// Got-IP callback
static bb_wifi_on_got_ip_cb_t s_on_got_ip_cb = NULL;

// Disconnect callback
static bb_wifi_on_disconnect_cb_t s_on_disconnect_cb = NULL;

// WiFi scan cache
static bb_wifi_ap_t s_cached_scan[WIFI_SCAN_MAX];
static int s_cached_scan_count = 0;
static volatile bool s_scan_in_progress = false;

// Cached AP info — updated on STA_CONNECTED event and by periodic refresh timer.
// bb_wifi_get_info reads this instead of calling esp_wifi_sta_get_ap_info (which
// takes the WiFi driver mutex and blocks when the driver is busy).
// s_ap_mux guards s_cached_ssid, s_cached_bssid, and s_cached_rssi; portMUX
// is safe from both task and event-handler contexts without FreeRTOS API calls.
static portMUX_TYPE s_ap_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_cached_ssid[33] = {0};
static uint8_t s_cached_bssid[6] = {0};
static int8_t s_cached_rssi = 0;
static bb_periodic_timer_t s_rssi_refresh_timer = NULL;

// Lazy ping session for bb_wifi_gateway_reachable.
// Created once on first use; reused across calls (esp_ping_start re-triggers).
static esp_ping_handle_t    s_ping_handle = NULL;
static SemaphoreHandle_t    s_ping_mutex  = NULL;  // guards s_ping_handle create/use
static volatile bool        s_ping_success = false;
static SemaphoreHandle_t    s_ping_done   = NULL;  // binary semaphore, given on result

static void rssi_refresh_work_fn(void *arg)
{
    (void)arg;
    if (!s_has_ip) return;
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        portENTER_CRITICAL(&s_ap_mux);
        s_cached_rssi = info.rssi;
        portEXIT_CRITICAL(&s_ap_mux);
    }
}

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
    s_ping_success = true;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_ping_done, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
    s_ping_success = false;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_ping_done, &hp);
    if (hp) portYIELD_FROM_ISR();
}

bb_err_t bb_wifi_ping(uint32_t target_addr, uint32_t timeout_ms, bool *out_reachable)
{
    if (!out_reachable) return BB_ERR_INVALID_ARG;

    // Create mutex and done-semaphore lazily (once).
    if (!s_ping_mutex) {
        s_ping_mutex = xSemaphoreCreateMutex();
        if (!s_ping_mutex) return BB_ERR_NO_MEM;
    }
    if (!s_ping_done) {
        s_ping_done = xSemaphoreCreateBinary();
        if (!s_ping_done) return BB_ERR_NO_MEM;
    }

    xSemaphoreTake(s_ping_mutex, portMAX_DELAY);

    // Create session on first use.
    if (!s_ping_handle) {
        ip_addr_t ip_addr;
#if LWIP_IPV6
        ip_addr.type = IPADDR_TYPE_V4;
        ip_addr.u_addr.ip4.addr = target_addr;
#else
        ip_addr.addr = target_addr;
#endif

        esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
        cfg.count      = 1;
        cfg.timeout_ms = timeout_ms;
        cfg.target_addr = ip_addr;
        cfg.task_stack_size = 3072;

        esp_ping_callbacks_t cbs = {
            .on_ping_success = ping_on_success,
            .on_ping_timeout = ping_on_timeout,
            .on_ping_end     = NULL,
            .cb_args         = NULL,
        };

        esp_err_t err = esp_ping_new_session(&cfg, &cbs, &s_ping_handle);
        if (err != ESP_OK) {
            xSemaphoreGive(s_ping_mutex);
            return err;
        }
    }

    // Drain any leftover semaphore token from a previous run.
    xSemaphoreTake(s_ping_done, 0);
    s_ping_success = false;

    esp_ping_start(s_ping_handle);
    // Wait for result with a margin over timeout_ms.
    TickType_t wait = pdMS_TO_TICKS(timeout_ms + 500);
    bool got = (xSemaphoreTake(s_ping_done, wait) == pdTRUE);
    *out_reachable = got && s_ping_success;

    xSemaphoreGive(s_ping_mutex);
    return BB_OK;
}

bool bb_wifi_gateway_reachable(uint32_t timeout_ms)
{
    if (!s_sta_netif) return false;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) return false;
    if (ip_info.gw.addr == 0) return false;

    bool reachable = false;
    bb_wifi_ping(ip_info.gw.addr, timeout_ms, &reachable);
    return reachable;
}

static void reconnect_work_fn(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

// Getters for diagnostics
void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us)
{
    if (wifi_reconn_is_active()) {
        wifi_reconn_get_disconnect(reason, age_us);
    } else {
        if (reason) *reason = 0;
        if (age_us) *age_us = 0;
    }
}

int bb_wifi_get_retry_count(void)
{
    return wifi_reconn_is_active() ? wifi_reconn_get_retry_count() : atomic_load(&s_retry_count);
}

bb_err_t bb_wifi_get_ip_str(char *out, size_t out_len)
{
    if (!s_sta_netif || out_len < 16) {
        if (out && out_len > 0) {
            snprintf(out, out_len, "0.0.0.0");
        }
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        snprintf(out, out_len, "0.0.0.0");
        return ESP_FAIL;
    }

    snprintf(out, out_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

bb_err_t bb_wifi_get_rssi(int8_t *out)
{
    if (!out) return ESP_FAIL;
    portENTER_CRITICAL(&s_ap_mux);
    *out = s_cached_rssi;
    portEXIT_CRITICAL(&s_ap_mux);
    return ESP_OK;
}

bool bb_wifi_has_ip(void)
{
    return s_has_ip;
}

uint32_t bb_wifi_get_lost_ip_count(void)
{
    return wifi_reconn_is_active() ? wifi_reconn_get_lost_ip_count() : 0;
}

uint32_t bb_wifi_get_lost_ip_age_s(void)
{
    if (!wifi_reconn_is_active()) return 0;
    int64_t age_us = wifi_reconn_get_lost_ip_age_us();
    return (uint32_t)(age_us / 1000000);
}

uint32_t bb_wifi_get_egress_dead_count(void)
{
    return wifi_reconn_is_active() ? wifi_reconn_get_egress_dead_count() : 0;
}

uint32_t bb_wifi_get_no_ip_count(void)
{
    return wifi_reconn_is_active() ? wifi_reconn_get_no_ip_count() : 0;
}

bb_err_t bb_wifi_get_info(bb_wifi_info_t *out)
{
    if (!out) return ESP_FAIL;
    memset(out, 0, sizeof(*out));

    portENTER_CRITICAL(&s_ap_mux);
    strncpy(out->ssid, s_cached_ssid, sizeof(out->ssid) - 1);
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    memcpy(out->bssid, s_cached_bssid, sizeof(out->bssid));
    out->rssi = s_cached_rssi;
    portEXIT_CRITICAL(&s_ap_mux);

    snprintf(out->ip, sizeof(out->ip), "0.0.0.0");
    bb_wifi_get_ip_str(out->ip, sizeof(out->ip));
    out->connected = s_has_ip;

    int64_t disc_age_us = 0;
    bb_wifi_get_disconnect(&out->disc_reason, &disc_age_us);
    out->disc_age_s = (uint32_t)(disc_age_us / 1000000);

    out->retry_count = bb_wifi_get_retry_count();
    return ESP_OK;
}

void bb_wifi_register_on_got_ip(bb_wifi_on_got_ip_cb_t cb)
{
    s_on_got_ip_cb = cb;
    // Late registration: fire immediately if STA already has an IP so the
    // caller doesn't miss the edge that fired before they wired up.
    if (cb && s_has_ip) {
        cb();
    }
}

void bb_wifi_register_on_disconnect(bb_wifi_on_disconnect_cb_t cb)
{
    s_on_disconnect_cb = cb;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Skip auto-connect during a controlled restart: bb_wifi_restart_sta() owns
        // the connect call to avoid racing with the FSM's explicit esp_wifi_connect().
        if (!s_sta_restarting) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e = (const wifi_event_sta_connected_t *)event_data;
        if (e) {
            size_t n = e->ssid_len < sizeof(s_cached_ssid) - 1 ? e->ssid_len : sizeof(s_cached_ssid) - 1;
            portENTER_CRITICAL(&s_ap_mux);
            memcpy(s_cached_ssid, e->ssid, n);
            s_cached_ssid[n] = '\0';
            memcpy(s_cached_bssid, e->bssid, sizeof(s_cached_bssid));
            portEXIT_CRITICAL(&s_ap_mux);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_has_ip = false;

        if (s_on_disconnect_cb) {
            s_on_disconnect_cb();
        }

        if (wifi_reconn_is_active()) {
            // Post-boot: manager task owns retry policy
            wifi_reconn_on_disconnect(disc ? disc->reason : 0);
            return;
        }

        // Boot-time connect: legacy inline retry until wifi_connect_sta() hands off
        if (atomic_load(&s_retry_count) >= WIFI_MAX_RETRY) {
            bb_log_w(TAG, "max retries reached, delaying 5s before retry");
            atomic_store(&s_retry_count, 0);
            if (!s_reconnect_timer) {
                bb_timer_deferred_oneshot_create(reconnect_work_fn, NULL,
                                                 "wifi_reconn_boot", &s_reconnect_timer);
            }
            bb_timer_oneshot_stop(s_reconnect_timer);
            bb_timer_oneshot_start(s_reconnect_timer, 5000000);
            return;
        }
        esp_wifi_connect();
        int rc = atomic_fetch_add(&s_retry_count, 1) + 1;
        bb_log_w(TAG, "retry %d/%d", rc, WIFI_MAX_RETRY);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        bb_log_i(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        atomic_store(&s_retry_count, 0);
        s_has_ip = true;
        if (wifi_reconn_is_active()) {
            wifi_reconn_on_got_ip();
        }
#if CONFIG_BB_WIFI_RECONFIGURE
        if (s_pending_try) {
            s_pending_try = false;
            bb_nv_config_commit_wifi_pending();
        }
#endif
        bb_nv_config_reset_boot_count();
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        // Invoke registered callback
        if (s_on_got_ip_cb) {
            s_on_got_ip_cb();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        esp_netif_ip_info_t ip_info;
        if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK
            && ip_info.ip.addr != 0) {
            bb_log_d(TAG, "IP_LOST_IP but netif still has IP, ignoring");
            return;
        }
        s_has_ip = false;
        bb_log_w(TAG, "IP lost");
        if (wifi_reconn_is_active()) {
            wifi_reconn_on_lost_ip();
        }
    }
}

void bb_wifi_restart_sta(void)
{
    bb_log_w(TAG, "STA restart: stop/start to clear wedged driver state");
    // s_sta_restarting suppresses the auto-connect in STA_START event handler.
    // wifi_reconn_absorb_next_disconnect suppresses the synthetic DISCONNECTED
    // that esp_wifi_stop() generates internally (so the FSM doesn't double-count).
    s_sta_restarting = true;
    wifi_reconn_absorb_next_disconnect();
    esp_wifi_stop();
    esp_wifi_start();
    // Re-apply saved config (inactive_time is not stored in flash; set again).
#if BB_WIFI_INACTIVE_TIME_ENABLE
    esp_wifi_set_inactive_time(WIFI_IF_STA, BB_WIFI_INACTIVE_TIME_S);
#endif
    esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
    s_sta_restarting = false;
    esp_wifi_connect();
}

bb_err_t bb_wifi_request_recovery(const char *reason)
{
    // No-op if we don't have IP yet — FSM owns recovery in that state.
    if (!bb_wifi_has_ip()) {
        bb_log_d(TAG, "request_recovery(%s): no IP, FSM owns recovery", reason ? reason : "");
        return BB_OK;
    }

    // Debounce: at most one recovery per cooldown window.
    static int64_t s_last_recovery_us = 0;
    int64_t now = (int64_t)esp_timer_get_time();
    int64_t cooldown_us = (int64_t)BB_WIFI_RECOVERY_COOLDOWN_S * 1000000LL;
    if (s_last_recovery_us != 0 && (now - s_last_recovery_us) < cooldown_us) {
        bb_log_i(TAG, "request_recovery(%s): debounced (cooldown %us)",
                 reason ? reason : "", (unsigned)BB_WIFI_RECOVERY_COOLDOWN_S);
        return BB_OK;
    }
    s_last_recovery_us = now;

    bb_log_i(TAG, "request_recovery: reason='%s', signaling reconn task", reason ? reason : "");
    if (wifi_reconn_is_active()) {
        wifi_reconn_request_recovery(reason);
    }
    return BB_OK;
}

void bb_wifi_force_reassociate(void)
{
    bb_log_w(TAG, "forcing WiFi reassociation (zombie state recovery)");
#if BB_WIFI_INACTIVE_TIME_ENABLE
    bb_wifi_restart_sta();
#else
    esp_wifi_disconnect();
#endif
}

bb_err_t bb_wifi_ensure_netif(void)
{
    if (s_netif_initialized) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_netif_initialized = true;
    return ESP_OK;
}

typedef enum { CREDS_LIVE, CREDS_PENDING } wifi_creds_src_t;

static esp_err_t wifi_connect_sta_ex(wifi_creds_src_t src, uint32_t timeout_ms,
                                     bool restart_on_timeout, bool is_pending_try)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(bb_wifi_ensure_netif());

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    // Apply the persisted hostname now that the STA netif exists but before
    // esp_wifi_start(), so the first DHCP DISCOVER carries the configured name.
    // esp_netif_set_hostname requires the netif to exist, so this cannot run
    // earlier (in autoinit) — DHCP/mDNS otherwise fall back to "espressif".
    const char *hn = bb_nv_config_hostname();
    if (hn && hn[0]) {
        esp_err_t hn_err = esp_netif_set_hostname(s_sta_netif, hn);
        if (hn_err != ESP_OK) {
            bb_log_w(TAG, "esp_netif_set_hostname failed (%d); continuing", (int)hn_err);
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (!s_wifi_handler) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &s_wifi_handler));
    }
    if (!s_ip_handler) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &s_ip_handler));
    }

    wifi_config_t wifi_config = {0};
#if CONFIG_BB_WIFI_RECONFIGURE
    if (src == CREDS_PENDING) {
        strncpy((char *)wifi_config.sta.ssid, bb_nv_config_wifi_pending_ssid(), sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, bb_nv_config_wifi_pending_pass(), sizeof(wifi_config.sta.password));
    } else {
#endif
        strncpy((char *)wifi_config.sta.ssid, bb_nv_config_wifi_ssid(), sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, bb_nv_config_wifi_pass(), sizeof(wifi_config.sta.password));
#if CONFIG_BB_WIFI_RECONFIGURE
    }
#else
    (void)src;
    (void)is_pending_try;
#endif
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    memcpy(&s_sta_config, &wifi_config, sizeof(s_sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
#if BB_WIFI_INACTIVE_TIME_ENABLE
    // Beacon-loss detection: driver emits WIFI_EVENT_STA_DISCONNECTED after this
    // many seconds without a beacon, flowing into the normal reconnect FSM path.
    // Must be called after esp_wifi_start(); minimum 3 for STA (ESP-IDF hard floor).
    esp_wifi_set_inactive_time(WIFI_IF_STA, BB_WIFI_INACTIVE_TIME_S);
#endif
#if CONFIG_BB_WIFI_PS_NONE
    esp_wifi_set_ps(WIFI_PS_NONE);
#elif CONFIG_BB_WIFI_PS_MAX_MODEM
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
#else
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif

    // Initialize RSSI refresh timer
    if (!s_rssi_refresh_timer) {
        bb_timer_deferred_periodic_create(rssi_refresh_work_fn, NULL,
                                          "bb_wifi_rssi", &s_rssi_refresh_timer);
    }
    if (s_rssi_refresh_timer) {
        bb_timer_periodic_stop(s_rssi_refresh_timer);
        bb_timer_periodic_start(s_rssi_refresh_timer, 5 * 1000 * 1000);  // 5s
    }

    bb_log_i(TAG, "connecting to %s", (char *)wifi_config.sta.ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;

        // Stop reconnect timer before deinit to prevent firing on dead driver
        if (s_reconnect_timer) {
            bb_timer_oneshot_stop(s_reconnect_timer);
        }

        // Clean up WiFi so it can be reinitialized
        esp_wifi_stop();
        esp_wifi_deinit();

        // Unregister event handlers and clear handles for fresh registration on retry
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_handler);
        s_wifi_handler = NULL;
        s_ip_handler = NULL;

        if (restart_on_timeout) {
            if (bb_ota_is_validated()) {
                bb_log_w(TAG, "wifi cold-boot timeout; firmware validated, returning ESP_ERR_TIMEOUT without reboot");
                return ESP_ERR_TIMEOUT;
            }
            bb_log_e(TAG, "WiFi connection timeout after 60s, restarting");
            bb_nv_config_increment_boot_count();
            esp_restart();
        } else {
            bb_log_e(TAG, "WiFi connection timeout after %us", (unsigned)(timeout_ms / 1000));
            return ESP_ERR_TIMEOUT;
        }
    }

    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    wifi_reconn_start();

    return ESP_OK;
}

bb_err_t bb_wifi_init(void)
{
    return wifi_connect_sta_ex(CREDS_LIVE, 60000, true, false);
}

bb_err_t bb_wifi_init_sta(void)
{
    return wifi_connect_sta_ex(CREDS_LIVE, 60000, false, false);
}

int bb_wifi_scan_networks(bb_wifi_ap_t *results, int max_results)
{
    if (!results || max_results <= 0) {
        return 0;
    }

    wifi_scan_config_t scan_config = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true); // blocking
    if (err != ESP_OK) {
        bb_log_e(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        return 0;
    }
    if (count > max_results) {
        count = max_results;
    }

    wifi_ap_record_t *records = malloc(count * sizeof(wifi_ap_record_t));
    if (!records) {
        return 0;
    }

    esp_wifi_scan_get_ap_records(&count, records);

    // Deduplicate and sort by RSSI (scan can return same SSID multiple times)
    int unique = 0;
    for (int i = 0; i < count && unique < max_results; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;  // skip hidden
        }

        // Check if already added
        bool dup = false;
        for (int j = 0; j < unique; j++) {
            if (strcmp(results[j].ssid, (char *)records[i].ssid) == 0) {
                dup = true;
                break;
            }
        }

        if (!dup) {
            memset(results[unique].ssid, 0, sizeof(results[unique].ssid));
            strncpy(results[unique].ssid, (char *)records[i].ssid, 32);
            results[unique].rssi = records[i].rssi;
            results[unique].secure = records[i].authmode != WIFI_AUTH_OPEN;
            unique++;
        }
    }

    free(records);
    return unique;
}

static void scan_worker_task(void *arg)
{
    (void)arg;
    int count = bb_wifi_scan_networks(s_cached_scan, WIFI_SCAN_MAX);
    // Release fence: ensure s_cached_scan[] writes are visible before the
    // count is published to readers.
    atomic_thread_fence(memory_order_release);
    s_cached_scan_count = count;
    s_scan_in_progress = false;

    bb_log_d(TAG, "async scan complete: %d networks found", count);
    vTaskDelete(NULL);
}

void bb_wifi_scan_start_async(void)
{
    if (s_scan_in_progress) {
        return;
    }
    s_scan_in_progress = true;

    BaseType_t xReturned = xTaskCreate(
        scan_worker_task,
        "wifi_scan",
        4096,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL
    );

    if (xReturned != pdPASS) {
        bb_log_w(TAG, "failed to create wifi_scan task");
        s_scan_in_progress = false;
    }
}

int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results)
{
    if (!results || max_results <= 0) {
        return 0;
    }

    // Acquire fence: pairs with the release fence in scan_worker_task so that
    // the s_cached_scan[] entries written before the count are visible here.
    atomic_thread_fence(memory_order_acquire);
    int count = s_cached_scan_count;
    if (count > max_results) {
        count = max_results;
    }
    if (count > 0) {
        memcpy(results, s_cached_scan, count * sizeof(bb_wifi_ap_t));
    }
    return count;
}

bb_err_t bb_wifi_set_hostname(const char *hostname)
{
    if (!hostname || !*hostname) return BB_ERR_INVALID_ARG;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return BB_ERR_INVALID_STATE;
    esp_err_t err = esp_netif_set_hostname(sta, hostname);
    return (err == ESP_OK) ? BB_OK : BB_ERR_INVALID_STATE;
}

#if CONFIG_BB_WIFI_RECONFIGURE
static void reconfig_reboot_work_fn(void *arg)
{
    (void)arg;
    esp_restart();
}

bb_err_t bb_wifi_reconfigure(const char *ssid, const char *pass)
{
    bb_err_t err = bb_wifi_pending_validate(ssid, pass);
    if (err != BB_OK) return err;

    err = bb_nv_config_set_wifi_pending(ssid, pass);
    if (err != BB_OK) return err;

    static bb_oneshot_timer_t s_reconfig_timer = NULL;
    if (!s_reconfig_timer) {
        bb_timer_deferred_oneshot_create(reconfig_reboot_work_fn, NULL,
                                         "bb_wifi_reconfig", &s_reconfig_timer);
    }
    bb_timer_oneshot_stop(s_reconfig_timer);
    bb_timer_oneshot_start(s_reconfig_timer, 500 * 1000); // 500 ms — lets HTTP 202 flush
    return BB_OK;
}
#else
bb_err_t bb_wifi_reconfigure(const char *ssid, const char *pass)
{
    (void)ssid;
    (void)pass;
    return BB_ERR_UNSUPPORTED;
}
#endif

#if CONFIG_BB_WIFI_AUTOREGISTER
static bb_err_t bb_wifi_autoinit(void)
{
    // No credentials → nothing to connect to. Return immediately so the
    // EARLY-tier walker continues and the consumer can branch into
    // provisioning mode (AP fallback).
    const char *ssid = bb_nv_config_wifi_ssid();
    if (!ssid || !ssid[0]) {
        bb_log_i(TAG, "bb_wifi_autoinit: no ssid configured; skipping connect");
        return BB_OK;
    }

#if CONFIG_BB_WIFI_RECONFIGURE
    // Pending-creds try: if a runtime reconfigure armed new credentials, attempt
    // to connect with them under a bounded timeout. Success commits them as live;
    // timeout discards them and reboots onto the untouched live creds WITHOUT
    // incrementing boot_count so the device does not drift toward AP-fallback.
    if (bb_nv_config_wifi_pending_active()) {
        s_pending_try = true;
        esp_err_t terr = wifi_connect_sta_ex(CREDS_PENDING,
            (uint32_t)CONFIG_BB_WIFI_PENDING_TRY_TIMEOUT_S * 1000,
            /*restart_on_timeout=*/false, /*is_pending_try=*/true);
        if (terr == ESP_OK) {
            // Commit already happened in the got-IP handler.
            return BB_OK;
        }
        s_pending_try = false;
        bb_nv_config_clear_wifi_pending();
        bb_log_w(TAG, "pending wifi try failed; reverting to saved network and rebooting");
        esp_restart();
        // unreachable — esp_restart does not return
    }
#endif

    // Hostname is applied inside wifi_connect_sta_ex() right after the STA netif
    // is created — esp_netif_set_hostname requires the netif to exist.
    bb_err_t err = bb_wifi_init_sta();

#if CONFIG_BB_WIFI_RETRY_FOREVER_WHEN_VALIDATED
    // Validated firmware should keep trying — a network outage shouldn't wipe
    // credentials or knock the device into AP mode. Unvalidated firmware
    // falls through to the existing boot-count / AP-fallback path that
    // bb_wifi already implements internally.
    //
    // Sleep is broken into 1 s chunks so the task WDT (default 5 s in ESP-IDF
    // v5.x, subscribed to app_main by default) is fed on every iteration and
    // the 30 s backoff doesn't trip it.
    while (err != BB_OK && bb_ota_is_validated()) {
        bb_log_w(TAG, "wifi cold-boot timeout; retrying in 30s");
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            bb_wdt_task_feed();
        }
        err = bb_wifi_init_sta();
    }
#endif

    if (err != BB_OK) {
        // Swallow the error so the EARLY-tier walker continues. Consumers that
        // need wifi state explicitly call bb_wifi_is_connected().
        bb_log_w(TAG, "bb_wifi_autoinit: connect failed (%d); continuing", (int)err);
    }
    return BB_OK;
}
BB_REGISTRY_REGISTER_EARLY(bb_wifi, bb_wifi_autoinit);
#endif

// Transport stubs — ESP-IDF bb_http uses esp_http_server directly.
bb_err_t bb_wifi_listen(uint16_t port) { (void)port; return BB_ERR_INVALID_STATE; }
bb_err_t bb_wifi_accept(bb_conn_t **out) { if (out) *out = NULL; return BB_ERR_INVALID_STATE; }
int  bb_conn_available(bb_conn_t *c) { (void)c; return 0; }
int  bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n) { (void)c; (void)buf; (void)n; return -1; }
int  bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n) { (void)c; (void)buf; (void)n; return -1; }
void bb_conn_close(bb_conn_t *c) { (void)c; }
