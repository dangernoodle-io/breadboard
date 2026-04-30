#include "bb_wifi.h"
#include "bb_nv.h"
#include "wifi_reconn.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "bb_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "bb_wifi";
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_MAX_RETRY 10

// State tracking
static bool s_netif_initialized = false;
static esp_netif_t *s_sta_netif = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static volatile bool s_has_ip = false;

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
static char s_cached_ssid[33] = {0};
static uint8_t s_cached_bssid[6] = {0};
static int8_t s_cached_rssi = 0;
static esp_timer_handle_t s_rssi_refresh_timer = NULL;

static void rssi_refresh_cb(void *arg)
{
    (void)arg;
    if (!s_has_ip) return;
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        s_cached_rssi = info.rssi;
    }
}

static void reconnect_timer_cb(void *arg)
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
    return wifi_reconn_is_active() ? wifi_reconn_get_retry_count() : s_retry_count;
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
    *out = s_cached_rssi;
    return ESP_OK;
}

bool bb_wifi_has_ip(void)
{
    return s_has_ip;
}

bb_err_t bb_wifi_get_info(bb_wifi_info_t *out)
{
    if (!out) return ESP_FAIL;
    memset(out, 0, sizeof(*out));

    strncpy(out->ssid, s_cached_ssid, sizeof(out->ssid) - 1);
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    memcpy(out->bssid, s_cached_bssid, sizeof(out->bssid));
    out->rssi = s_cached_rssi;

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
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e = (const wifi_event_sta_connected_t *)event_data;
        if (e) {
            size_t n = e->ssid_len < sizeof(s_cached_ssid) - 1 ? e->ssid_len : sizeof(s_cached_ssid) - 1;
            memcpy(s_cached_ssid, e->ssid, n);
            s_cached_ssid[n] = '\0';
            memcpy(s_cached_bssid, e->bssid, sizeof(s_cached_bssid));
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
        if (s_retry_count >= WIFI_MAX_RETRY) {
            bb_log_w(TAG, "max retries reached, delaying 5s before retry");
            s_retry_count = 0;
            if (!s_reconnect_timer) {
                const esp_timer_create_args_t args = {
                    .callback = reconnect_timer_cb,
                    .name = "wifi_reconn_boot",
                };
                esp_timer_create(&args, &s_reconnect_timer);
            }
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, 5000000);
            return;
        }
        esp_wifi_connect();
        s_retry_count++;
        bb_log_w(TAG, "retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        bb_log_i(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_has_ip = true;
        if (wifi_reconn_is_active()) {
            wifi_reconn_on_got_ip();
        }
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
    }
}

void bb_wifi_force_reassociate(void)
{
    bb_log_w(TAG, "forcing WiFi reassociation (zombie state recovery)");
    esp_wifi_disconnect();
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

static esp_err_t wifi_connect_sta(bool restart_on_timeout)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(bb_wifi_ensure_netif());

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
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
    strncpy((char *)wifi_config.sta.ssid, bb_nv_config_wifi_ssid(), sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, bb_nv_config_wifi_pass(), sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // Initialize RSSI refresh timer
    if (!s_rssi_refresh_timer) {
        const esp_timer_create_args_t args = {
            .callback = rssi_refresh_cb,
            .name = "bb_wifi_rssi",
        };
        esp_timer_create(&args, &s_rssi_refresh_timer);
    }
    if (s_rssi_refresh_timer) {
        esp_timer_stop(s_rssi_refresh_timer);
        esp_timer_start_periodic(s_rssi_refresh_timer, 5 * 1000 * 1000);  // 5s
    }

    bb_log_i(TAG, "connecting to %s", bb_nv_config_wifi_ssid());
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;

        // Stop reconnect timer before deinit to prevent firing on dead driver
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
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
            bb_log_e(TAG, "WiFi connection timeout after 60s, restarting");
            bb_nv_config_increment_boot_count();
            esp_restart();
        } else {
            bb_log_e(TAG, "WiFi connection timeout after 60s");
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
    return wifi_connect_sta(true);
}

bb_err_t bb_wifi_init_sta(void)
{
    return wifi_connect_sta(false);
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

// Transport stubs — ESP-IDF bb_http uses esp_http_server directly.
bb_err_t bb_wifi_listen(uint16_t port) { (void)port; return BB_ERR_INVALID_STATE; }
bb_err_t bb_wifi_accept(bb_conn_t **out) { if (out) *out = NULL; return BB_ERR_INVALID_STATE; }
int  bb_conn_available(bb_conn_t *c) { (void)c; return 0; }
int  bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n) { (void)c; (void)buf; (void)n; return -1; }
int  bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n) { (void)c; (void)buf; (void)n; return -1; }
void bb_conn_close(bb_conn_t *c) { (void)c; }
