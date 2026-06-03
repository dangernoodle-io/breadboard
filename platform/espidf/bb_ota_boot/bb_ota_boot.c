#include "bb_ota_boot.h"
#include "bb_log.h"
#include "bb_nv.h"

// Flag + breadcrumb live in the bb_cfg namespace (generic bb_nv API). The flag
// is a one-shot mechanism marker, NOT a user config key — deliberately not in
// the bb_nv_config manifest table.
#define OTA_BOOT_NS         "bb_cfg"
#define OTA_BOOT_FLAG_KEY   "ota_boot_mode"
#define OTA_BOOT_STAGE_KEY  "ota_boot_stg"

static bb_ota_progress_cb_t s_progress_cb = NULL;

void bb_ota_boot_set_progress_cb(bb_ota_progress_cb_t cb)
{
    s_progress_cb = cb;
}

void bb_ota_boot_arm(void)
{
    bb_nv_set_u8(OTA_BOOT_NS, OTA_BOOT_FLAG_KEY, 1);
}

bool bb_ota_boot_pending(void)
{
    uint8_t v = 0;
    bb_nv_get_u8(OTA_BOOT_NS, OTA_BOOT_FLAG_KEY, &v, 0);
    return v != 0;
}

#ifdef ESP_PLATFORM

#include "bb_ntp.h"
#include "bb_wifi.h"
#include "bb_ota_pull.h"
#include "bb_update_check.h"
#include "bb_http.h"
#include "bb_registry.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb_ota_boot";

#define OTA_BOOT_UDP_PORT      9999
#define OTA_BOOT_WORKER_STACK  12288

// Resolve+pull params handed to the worker (single boot-mode run, statics safe).
static const char *s_boot_url   = NULL;
static const char *s_boot_board = NULL;

static void breadcrumb(uint8_t stage)
{
    bb_nv_set_u8(OTA_BOOT_NS, OTA_BOOT_STAGE_KEY, stage);
}

static void ota_boot_progress(bb_ota_phase_t phase, int pct)
{
    bb_ota_progress_cb_t cb = s_progress_cb;
    if (cb) cb(phase, pct);
}

// Boot-mode worker: resolve the latest asset + pull it, both at full heap. Runs
// on a fat stack (the mbedTLS handshake in bb_update_check_now needs >=8 KB, more
// than the main task carries). NEVER returns — always reboots.
static void ota_boot_worker(void *arg)
{
    (void)arg;

    // Stand up bb_update_check synchronously — init + now() run on this stack,
    // no persistent worker spawned, so the runtime can keep it autoregister-off.
    if (bb_update_check_init(NULL) != BB_OK ||
        bb_update_check_set_releases_url(s_boot_url) != BB_OK ||
        bb_update_check_set_firmware_board(s_boot_board) != BB_OK) {
        bb_log_e(TAG, "boot-mode: update_check setup failed, rebooting normal");
        breadcrumb(0xE2);
        ota_boot_progress(BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }

    bb_update_check_status_t st;
    if (bb_update_check_now() != BB_OK ||
        bb_update_check_get_status(&st) != BB_OK || !st.last_check_ok) {
        bb_log_e(TAG, "boot-mode: manifest check failed, rebooting normal");
        breadcrumb(0xE2);
        ota_boot_progress(BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }
    if (!st.available || st.download_url[0] == '\0') {
        bb_log_w(TAG, "boot-mode: no update available, rebooting normal");
        breadcrumb(0xE3);
        ota_boot_progress(BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }
    breadcrumb(4);

    // run_sync drives START/PROGRESS/SUCCESS/FAIL through the forwarded cb.
    bb_log_w(TAG, "boot-mode: pulling %s", st.download_url);
    bb_err_t err = bb_ota_pull_run_sync(st.download_url);
    if (err == BB_OK) {
        breadcrumb(8);
        bb_log_w(TAG, "boot-mode: image written, rebooting to new image");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    bb_log_e(TAG, "boot-mode: pull failed (%s), rebooting normal", esp_err_to_name(err));
    breadcrumb(0xE4);
    esp_restart();
}

void bb_ota_boot_run_if_pending(const char *releases_url, const char *board)
{
    if (!bb_ota_boot_pending()) {
        return;
    }
    // Clear the one-shot flag FIRST so a failed pull falls back to a normal boot
    // on the next reset (no boot loop).
    bb_nv_set_u8(OTA_BOOT_NS, OTA_BOOT_FLAG_KEY, 0);
    breadcrumb(1);
    ota_boot_progress(BB_OTA_PHASE_START, 0);
    bb_log_w(TAG, "OTA boot-mode: pulling firmware at full heap");

    // Wait for the STA link (caller started WiFi; IP may not be bound yet).
    for (int i = 0; i < 60 && !bb_wifi_has_ip(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!bb_wifi_has_ip()) {
        bb_log_e(TAG, "OTA boot-mode: no wifi, rebooting normal");
        breadcrumb(0xE1);
        ota_boot_progress(BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }
    breadcrumb(2);

#if CONFIG_BB_LOG_UDP_SINK
    // Broadcast the boot-mode log trace (serial-less boards). 0xFFFFFFFF =
    // 255.255.255.255 (subnet broadcast). Listen with: nc -ul <port>.
    bb_log_udp_enable(0xFFFFFFFFU, OTA_BOOT_UDP_PORT);
#endif

    // TLS cert validation needs a real clock — a 1970 epoch reads as "cert not
    // yet valid". Start NTP and wait; proceed regardless so a sync miss surfaces
    // as a visible cert error rather than a silent hang.
    bb_ntp_start(NULL);
    if (!bb_ntp_wait_synced(15000)) {
        bb_log_w(TAG, "OTA boot-mode: SNTP timeout, proceeding (TLS may fail)");
    }
    breadcrumb(3);

    // Hand the resolve + pull to a fat-stack worker (the TLS handshakes need more
    // stack than the main task carries); forward the progress cb so the download
    // drives the LED. This task then blocks; the worker always reboots.
    s_boot_url   = releases_url;
    s_boot_board = board;
    bb_ota_pull_set_progress_cb(s_progress_cb);
    if (xTaskCreate(ota_boot_worker, "ota_boot", OTA_BOOT_WORKER_STACK, NULL, 5, NULL) != pdPASS) {
        bb_log_e(TAG, "OTA boot-mode: worker create failed, rebooting normal");
        breadcrumb(0xE5);
        ota_boot_progress(BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

/* Reboot shortly after the route's 202 response flushes. */
static void ota_boot_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// POST /api/update/boot — arm boot mode and reboot. The lean trigger: one route,
// no download worker until the next boot.
static bb_err_t ota_boot_handler(bb_http_request_t *req)
{
    bb_ota_boot_arm();
    bb_log_w(TAG, "OTA boot-mode armed via /api/update/boot; rebooting");
    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting_for_boot_mode_ota");
    bb_http_resp_json_obj_end(&obj);
    xTaskCreate(ota_boot_reboot_task, "ota_boot_rb", 2048, NULL, 5, NULL);
    return BB_OK;
}

bb_err_t bb_ota_boot_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    static const bb_route_response_t s_responses[] = {
        { 202, "application/json",
          "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}}}",
          "boot mode armed; rebooting" },
        { 0 },
    };
    static const bb_route_t s_route = {
        .method    = BB_HTTP_POST,
        .path      = "/api/update/boot",
        .tag       = "update",
        .summary   = "Arm OTA boot mode and reboot (full-heap pull next boot)",
        .responses = s_responses,
        .handler   = ota_boot_handler,
    };

    bb_err_t rc = bb_http_register_described_route(server, &s_route);
    if (rc != BB_OK) {
        bb_log_e(TAG, "register /api/update/boot failed: %d", rc);
        return rc;
    }
    bb_log_i(TAG, "OTA boot route registered");
    return BB_OK;
}

#if CONFIG_BB_OTA_BOOT_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_ota_boot, bb_ota_boot_init, 1);
#endif

#endif // ESP_PLATFORM
