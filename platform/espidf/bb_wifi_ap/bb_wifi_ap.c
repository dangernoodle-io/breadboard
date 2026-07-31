// ESP-IDF glue for bb_wifi_ap: SoftAP bring-up/teardown + the captive-DNS
// socket task loop. Lifted out of bb_prov's bb_prov_start_ap()/
// bb_prov_stop_ap()/dns_task() (KB 781) -- the pure DNS packet building and
// SSID derivation now live in components/bb_wifi_ap/src/bb_wifi_ap_core.c.
#include "bb_wifi_ap.h"
#include "bb_wifi.h"
#include "bb_log.h"
#include "bb_str.h"
#include "bb_task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "bb_wifi_ap";

// Bound on bb_wifi_ap_stop()'s wait for an in-flight bb_wifi scan to go
// idle before touching the esp_wifi driver mode/lifecycle -- see the
// bb_wifi_scan_wait_idle() call in bb_wifi_ap_stop() below. Never blocks
// teardown forever: on timeout, teardown proceeds anyway (logged).
#define BB_WIFI_AP_STOP_SCAN_WAIT_MS 5000

// Second-chance bound after esp_wifi_scan_stop() is used to cooperatively
// cancel a still-in-flight scan on the BB_WIFI_AP_STOP_SCAN_WAIT_MS timeout
// path (see bb_wifi_ap_stop() below). Short: esp_wifi_scan_stop() forces the
// blocking esp_wifi_scan_start(..., true) call on the scan task to return,
// so the scan task only needs enough time to observe that and run its
// cleanup (scan_worker_task in bb_wifi.c) -- not another full scan interval.
#define BB_WIFI_AP_STOP_SCAN_CANCEL_WAIT_MS 1000

// AP + captive-DNS state.
static esp_netif_t *s_ap_netif = NULL;
static volatile bool s_dns_running = false;
static TaskHandle_t s_dns_task_handle = NULL;
static char s_ap_ssid[32];

// AP SSID prefix (default "BB-").
static char s_ap_ssid_prefix[16] = "BB-";
static bool s_ap_ssid_prefix_set = false;
// AP password (default "breadboard").
static char s_ap_password[64] = "breadboard";

// Captive-DNS task: answers every UDP:53 query with an A record pointing at
// the AP's own address (192.168.4.1, the esp_netif AP default). Packet
// building is the pure bb_wifi_ap_dns_build_response() (host-tested).
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        bb_log_e(TAG, "failed to create DNS socket");
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        bb_log_e(TAG, "failed to bind DNS socket");
        close(sock);
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Set receive timeout so we can check s_dns_running.
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bb_log_i(TAG, "captive DNS listening on 0.0.0.0:53");

    static const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    uint8_t buf[256];
    uint8_t resp[sizeof(buf) + 16];  // +16 = DNS answer trailer, see bb_wifi_ap_dns_build_response
    while (s_dns_running) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &client_len);
        if (len <= 0) {
            continue;  // timeout or error
        }

        int resp_len = bb_wifi_ap_dns_build_response(buf, len, answer_ip, resp, sizeof(resp));
        if (resp_len <= 0) {
            continue;  // too short to be a DNS query, or would overflow resp
        }

        sendto(sock, resp, resp_len, 0, (struct sockaddr *)&client, client_len);
    }

    close(sock);
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

bb_err_t bb_wifi_ap_start(void)
{
    if (s_ap_netif != NULL) {
        bb_log_w(TAG, "AP already started");
        return BB_OK;
    }

    // Idempotent net-stack bring-up, matching bb_wifi_ensure_net_stack()'s
    // contract but inlined here so bb_wifi_ap does not hard-depend on
    // bb_wifi (both talk to the same single esp_netif/esp_wifi driver via
    // the ESP-IDF SDK, not via each other).
    esp_err_t stack_err = esp_netif_init();
    if (stack_err != ESP_OK && stack_err != ESP_ERR_INVALID_STATE) {
        bb_log_e(TAG, "esp_netif_init failed: %s", esp_err_to_name(stack_err));
        return stack_err;
    }
    stack_err = esp_event_loop_create_default();
    if (stack_err != ESP_OK && stack_err != ESP_ERR_INVALID_STATE) {
        bb_log_e(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(stack_err));
        return stack_err;
    }

    // Create AP netif with default config (auto-starts DHCPS).
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        bb_log_e(TAG, "failed to create AP netif");
        return ESP_FAIL;
    }

    // Mode coordination (KB 781 crux): read the esp_wifi driver's current
    // mode before touching it. esp_wifi_get_mode() returns
    // ESP_ERR_WIFI_NOT_INIT when esp_wifi_init() has never run -- treat
    // that as "first bring-up" (the AP-fallback/provisioning-only case);
    // any other successful read means a STA session already owns the
    // driver and must not be clobbered.
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    esp_err_t mode_err = esp_wifi_get_mode(&cur_mode);
    bool driver_inited = (mode_err == ESP_OK);
    if (!driver_inited && mode_err != ESP_ERR_WIFI_NOT_INIT) {
        bb_log_e(TAG, "esp_wifi_get_mode failed: %s", esp_err_to_name(mode_err));
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
        return mode_err;
    }

    if (!driver_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        cur_mode = WIFI_MODE_NULL;
    }

    bool had_sta = (cur_mode == WIFI_MODE_STA || cur_mode == WIFI_MODE_APSTA);
    wifi_mode_t new_mode = had_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_ERROR_CHECK(esp_wifi_set_mode(new_mode));

    // Initialize WiFi and set mode before reading AP MAC
    // (hosted/wifi_remote populates per-interface MACs after mode is set;
    // reading ESP_MAC_WIFI_SOFTAP before set_mode returns
    // ESP_ERR_NOT_SUPPORTED on P4+C6).
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    const char *prefix = s_ap_ssid_prefix_set ? s_ap_ssid_prefix : "BB-";
    char ssid[32];
    if (bb_wifi_ap_build_ssid(prefix, mac, ssid, sizeof(ssid)) != BB_OK) {
        bb_log_e(TAG, "failed to build AP SSID");
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
        return BB_ERR_INVALID_ARG;
    }
    bb_strlcpy(s_ap_ssid, ssid, sizeof(s_ap_ssid));

    // Configure AP.
    wifi_config_t ap_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    bb_str_field((char *)ap_config.ap.password, s_ap_password, sizeof(ap_config.ap.password));
    bb_str_field((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (!driver_inited) {
        // First bring-up: nothing else has called esp_wifi_start() yet.
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    // Else: the driver is already started (STA active). Setting the mode
    // to APSTA above brings the AP interface up without a re-start.

    // Start DNS task.
    s_dns_running = true;
    bb_task_config_t dns_cfg = {
        .entry       = dns_task,
        .name        = "dns",
        .arg         = NULL,
        .stack_bytes = 4096,
        .priority    = tskIDLE_PRIORITY + 1,
        .core        = 0,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    if (bb_task_create(&dns_cfg, (void **)&s_dns_task_handle) != BB_OK) {
        bb_log_e(TAG, "failed to create DNS task");
        s_dns_running = false;
        if (!driver_inited) {
            esp_wifi_stop();
            esp_wifi_deinit();
        } else {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
        return ESP_FAIL;
    }

    // Never log the password value in plaintext -- bb_log_secret is the
    // only sanctioned path for a secret to reach a log line (see bb_log.h).
    // Defence-in-depth here specifically: the default AP password
    // ("breadboard", see s_ap_password's initializer above) is a build-time
    // constant anyone with the firmware image can already read, so this is
    // about consistent masking policy, not hiding a true secret -- a
    // user-set non-default password gets the same treatment either way.
    bb_log_i(TAG, "AP started: SSID=%s", ssid);
    bb_log_secret(BB_LOG_LEVEL_INFO, TAG, "password", s_ap_password);

    return ESP_OK;
}

void bb_wifi_ap_stop(void)
{
    if (s_ap_netif == NULL) {
        bb_log_w(TAG, "AP not started");
        return;
    }

    // Stop DNS task.
    s_dns_running = false;
    if (s_dns_task_handle != NULL) {
        // Wait for task to exit with a timeout.
        for (int i = 0; i < 50; i++) {  // 5 second timeout (50 * 100ms)
            if (s_dns_task_handle == NULL) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // Wait for any in-flight bb_wifi scan to go idle before touching the
    // driver mode/lifecycle below (CRITICAL, B1-809): bb_wifi_scan_networks()
    // can run a BLOCKING esp_wifi_scan_start() on a detached task
    // (wifi_ensure_scan_capable() may even promote AP -> APSTA to make the
    // scan possible in the first place -- see the mode re-query below).
    // esp_wifi_stop()/esp_wifi_deinit() with a scan in flight is unsupported
    // by the ESP-IDF driver and can assert/crash, not merely error-return.
    // Bounded: never blocks teardown forever -- on timeout, proceed anyway
    // (logged) rather than wedge provisioning.
    if (!bb_wifi_scan_wait_idle(BB_WIFI_AP_STOP_SCAN_WAIT_MS)) {
        // Timeout path only: cooperatively cancel the stalled scan before
        // falling through to teardown, narrowing the residual window where
        // teardown still proceeds with a scan possibly in flight.
        // esp_wifi_scan_stop() is the documented cancel API for an in-flight
        // esp_wifi_scan_start() (including the blocking variant used by
        // bb_wifi_scan_networks() -- it forces the call to return on the
        // scan task); its result here is logged only, never gated on, since
        // a benign race (the scan finishing on its own between the timeout
        // and this call) is expected and harmless either way.
        esp_err_t stop_err = esp_wifi_scan_stop();
        bb_log_w(TAG, "scan still in progress after %ums wait; sent esp_wifi_scan_stop() (%s)",
                 (unsigned)BB_WIFI_AP_STOP_SCAN_WAIT_MS, esp_err_to_name(stop_err));

        // Short second chance for the cancelled scan to actually go idle.
        // Bounded like the wait above: teardown must never hang forever, so
        // this proceeds regardless of the outcome -- only the log line
        // differs, so a field log can tell "cancelled then idle" from
        // "cancelled and still not idle, proceeding anyway".
        if (bb_wifi_scan_wait_idle(BB_WIFI_AP_STOP_SCAN_CANCEL_WAIT_MS)) {
            bb_log_i(TAG, "scan cancelled and went idle; proceeding with AP teardown");
        } else {
            bb_log_w(TAG, "scan cancelled but still not idle after %ums; proceeding with AP teardown anyway",
                     (unsigned)BB_WIFI_AP_STOP_SCAN_CANCEL_WAIT_MS);
        }
    }

    // Mode coordination: drop AP without tearing down an active STA.
    // Re-query the driver's LIVE mode here rather than trusting a start-time
    // snapshot -- bb_wifi_scan_networks() (wifi_ensure_scan_capable(),
    // B1-809) can promote AP -> APSTA behind this component's back any time
    // after bb_wifi_ap_start() ran, which a start-time-only snapshot would
    // miss entirely (this now happens on EVERY blank-NVS provisioning
    // session whose portal loads the SSID list). Mirrors
    // bb_wifi_ap_start()'s own fresh esp_wifi_get_mode() query above -- stop
    // is now symmetric with start.
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    esp_err_t mode_err = esp_wifi_get_mode(&cur_mode);
    bool has_sta = (mode_err == ESP_OK)
                   && (cur_mode == WIFI_MODE_STA || cur_mode == WIFI_MODE_APSTA);
    if (has_sta) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    } else {
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_deinit());
    }

    // Destroy AP netif.
    esp_netif_destroy(s_ap_netif);
    s_ap_netif = NULL;

    bb_log_i(TAG, "AP stopped");
}

void bb_wifi_ap_get_ssid(char *buf, size_t len)
{
    bb_strlcpy(buf, s_ap_ssid, len);
}

void bb_wifi_ap_set_ssid_prefix(const char *prefix)
{
    bb_wifi_ap_normalize_prefix(prefix, s_ap_ssid_prefix, sizeof(s_ap_ssid_prefix));
    s_ap_ssid_prefix_set = (prefix != NULL);
}

void bb_wifi_ap_set_password(const char *password)
{
    bb_wifi_ap_normalize_password(password, s_ap_password, sizeof(s_ap_password));
}
