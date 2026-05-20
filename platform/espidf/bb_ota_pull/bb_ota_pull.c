#include "bb_ota_pull.h"
#include "bb_update_check.h"
#include "bb_release_manifest.h"
#include "bb_http_client.h"
#include "bb_json.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Pluggable pause/resume callbacks
static bb_ota_pause_cb_t s_pause_cb = NULL;
static bb_ota_resume_cb_t s_resume_cb = NULL;

// Pluggable skip-check callback
static bb_ota_skip_check_cb_t s_skip_check_cb = NULL;

// Releases URL — caller must set before bb_ota_pull_check_now()
static char s_releases_url[512] = "";

// Firmware board name
static char s_firmware_board[64] = "";

// Per-recv HTTP timeout for OTA download (ms). Consumer-tunable via
// bb_ota_pull_set_http_timeout_ms(). Default 20 s matches the original
// hard-coded value; pass 0 to restore the default.
#define BB_OTA_HTTP_TIMEOUT_MS_DEFAULT 20000
static uint32_t s_http_timeout_ms = BB_OTA_HTTP_TIMEOUT_MS_DEFAULT;

void bb_ota_pull_set_http_timeout_ms(uint32_t ms)
{
    s_http_timeout_ms = (ms == 0) ? BB_OTA_HTTP_TIMEOUT_MS_DEFAULT : ms;
}

#ifndef ESP_PLATFORM
#include "bb_ota_pull_test_hooks.h"
uint32_t bb_ota_pull_host_get_http_timeout_ms(void)
{
    return s_http_timeout_ms;
}
#endif

#ifdef ESP_PLATFORM
#include "bb_http.h"
#include "bb_log.h"
#include "bb_registry.h"
#include "bb_wifi.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "bb_ota_pull";

#define OTA_TASK_STACK 12288
#define OTA_TASK_PRIO  3

static volatile bool s_ota_in_progress = false;
static int s_ota_task_core = 1;  // default: Core 1 (bitaxe-friendly, frees Core 0 for httpd/stratum)

#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_ota_pm_lock = NULL;
#endif

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_COMPLETE,
    OTA_STATE_ERROR,
} ota_state_t;

static const char *s_ota_state_names[] = {
    [OTA_STATE_IDLE]               = "idle",
    [OTA_STATE_CHECKING]           = "checking",
    [OTA_STATE_DOWNLOADING]        = "downloading",
    [OTA_STATE_VERIFYING]          = "verifying",
    [OTA_STATE_COMPLETE]           = "complete",
    [OTA_STATE_ERROR]              = "error",
};

typedef struct {
    ota_state_t state;
    char last_error[128];
    int progress_pct;
} ota_status_t;

static ota_status_t s_ota_status = {
    .state = OTA_STATE_IDLE,
    .last_error = {0},
    .progress_pct = 0,
};

// Spinlock protecting s_ota_status from concurrent access on dual-core ESP32-S3
static portMUX_TYPE s_ota_status_mux = portMUX_INITIALIZER_UNLOCKED;

static void ota_pm_lock_acquire(void)
{
#ifdef CONFIG_PM_ENABLE
    if (!s_ota_pm_lock) {
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "bb_ota_pull", &s_ota_pm_lock);
    }
    if (s_ota_pm_lock) {
        esp_pm_lock_acquire(s_ota_pm_lock);
    }
#endif
}

static void ota_pm_lock_release(void)
{
#ifdef CONFIG_PM_ENABLE
    if (s_ota_pm_lock) {
        esp_pm_lock_release(s_ota_pm_lock);
    }
#endif
}

static void ota_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    taskENTER_CRITICAL(&s_ota_status_mux);
    s_ota_status.state = OTA_STATE_ERROR;
    vsnprintf(s_ota_status.last_error, sizeof(s_ota_status.last_error), fmt, ap);
    taskEXIT_CRITICAL(&s_ota_status_mux);
    va_end(ap);
}

typedef struct {
    char latest_tag[32];
    char asset_url[256];
} ota_worker_arg_t;

#endif // ESP_PLATFORM

// Public API: Set pause/resume callbacks
void bb_ota_pull_set_hooks(bb_ota_pause_cb_t pause, bb_ota_resume_cb_t resume)
{
    s_pause_cb = pause;
    s_resume_cb = resume;
}

// Public API: Set skip-check callback
void bb_ota_pull_set_skip_check_cb(bb_ota_skip_check_cb_t cb)
{
    s_skip_check_cb = cb;
}

// Public API: Set releases URL
void bb_ota_pull_set_releases_url(const char *url)
{
    if (url) {
        strncpy(s_releases_url, url, sizeof(s_releases_url) - 1);
        s_releases_url[sizeof(s_releases_url) - 1] = '\0';
    } else {
        s_releases_url[0] = '\0';
    }
}

// Public API: Set firmware board
void bb_ota_pull_set_firmware_board(const char *board)
{
    if (board) {
        strncpy(s_firmware_board, board, sizeof(s_firmware_board) - 1);
        s_firmware_board[sizeof(s_firmware_board) - 1] = '\0';
    } else {
        s_firmware_board[0] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Portable manifest-fetch — no ESP-IDF types; compiled on host and device.
// ---------------------------------------------------------------------------

static bb_err_t ota_manifest_chunk_cb(void *cv, const char *data, size_t len)
{
    return bb_release_manifest_parse_github_stream_feed(
        (bb_release_manifest_stream_ctx_t *)cv, data, len);
}

/**
 * Fetch and stream-parse the release manifest.
 *
 * Fills out_tag and out_url on success. The board name used for asset
 * matching is taken from s_firmware_board (falls back to "unknown").
 *
 * Returns BB_OK, BB_ERR_INVALID_STATE (transport/HTTP error), or
 * BB_ERR_NOT_FOUND (parse: tag/asset missing).
 */
static bb_err_t ota_fetch_manifest(char *out_tag, size_t tag_cap,
                                   char *out_url, size_t url_cap)
{
    const char *board = s_firmware_board[0] != '\0' ? s_firmware_board : "unknown";

    bb_release_manifest_stream_ctx_t stream_ctx;
    bb_err_t perr = bb_release_manifest_parse_github_stream_begin(
        &stream_ctx, board, out_tag, tag_cap, out_url, url_cap);
    if (perr != BB_OK) {  // LCOV_EXCL_BR_LINE — args always valid here
        return perr;      // LCOV_EXCL_LINE
    }

    bb_http_client_cfg_t cfg = {
        .timeout_ms    = 10000,
        .max_attempts  = 3,
        .buffer_size   = 4096,
#ifdef ESP_PLATFORM
        .user_agent    = esp_app_get_description()->project_name,
#else
        .user_agent    = "bb_ota_pull/0.1",
#endif
        .accept_header = "application/vnd.github+json",
    };
    bb_http_client_result_t http_res = {0};

    bb_err_t herr = bb_http_client_get_stream(s_releases_url,
                                              ota_manifest_chunk_cb, &stream_ctx,
                                              &cfg, &http_res);

    bb_err_t end_err = bb_release_manifest_parse_github_stream_end(&stream_ctx);

    if (herr != BB_OK) {
        return BB_ERR_INVALID_STATE;
    }
    if (http_res.status_code != 200) {
        return BB_ERR_INVALID_STATE;
    }
    if (end_err != BB_OK) {
        return end_err;
    }
    return BB_OK;
}

#ifdef BB_OTA_PULL_TESTING
// Test hook: run ota_fetch_manifest with the currently configured URL/board.
bb_err_t bb_ota_pull_fetch_manifest_for_test(char *out_tag, size_t tag_cap,
                                             char *out_url, size_t url_cap)
{
    return ota_fetch_manifest(out_tag, tag_cap, out_url, url_cap);
}
#endif

#ifdef ESP_PLATFORM

/**
 * Reconfigure the task WDT timeout while preserving the idle-task mask and
 * panic-on-timeout policy from the firmware's Kconfig defaults.
 *
 * Used to bracket the OTA flash phase: extends the timeout so cache_disable
 * windows + consumer pause windows don't trip WDT-subscribed tasks (idle
 * tasks, consumer mining tasks). Restored to CONFIG_ESP_TASK_WDT_TIMEOUT_S
 * on every worker exit path.
 */
static void ota_wdt_set_timeout(uint32_t timeout_s)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_s * 1000U,
        .idle_core_mask =
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
            (1U << 0) |
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
            (1U << 1) |
#endif
            0U,
        .trigger_panic =
#if defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC
            true,
#else
            false,
#endif
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        bb_log_w(TAG, "esp_task_wdt_reconfigure(%ums): %s",
                 (unsigned)cfg.timeout_ms, esp_err_to_name(err));
    }
}

static void ota_wdt_extend(void)
{
    ota_wdt_set_timeout(CONFIG_BB_OTA_PULL_WDT_EXTENDED_S);
}

static void ota_wdt_restore(void)
{
    ota_wdt_set_timeout(CONFIG_ESP_TASK_WDT_TIMEOUT_S);
}

/**
 * Re-subscribe to the task WDT and delete the calling task.
 * Called at every exit path of worker tasks that removed themselves from
 * the WDT at entry. Best-effort: error is ignored — the task is dying.
 */
static void ota_task_exit(void)
{
    ota_wdt_restore();
    esp_task_wdt_add(NULL);
    vTaskDelete(NULL);
}

/**
 * OTA worker task - performs the actual firmware update.
 */
static void ota_worker_task(void *arg)
{
    // Remove this task from the task WDT. OTA is single-shot and long-running;
    // a stalled mbedtls recv should surface as a clean HTTP timeout, not a
    // WDT panic. ota_task_exit() re-adds before dying.
    esp_err_t wdt_err = esp_task_wdt_delete(NULL);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_NOT_FOUND) {
        bb_log_w(TAG, "esp_task_wdt_delete: %s", esp_err_to_name(wdt_err));
    }

    ota_worker_arg_t result;
    if (arg) {
        memcpy(&result, arg, sizeof(ota_worker_arg_t));
        free(arg);
    } else {
        ota_task_exit();
        return;
    }

    // Cooperatively pause work to free memory for OTA download
    bool paused = false;
    if (s_pause_cb) {
        paused = s_pause_cb();
        if (!paused) {
            bb_log_e(TAG, "mining pause failed — aborting OTA to avoid tls contention");
            ota_set_error("mining pause failed");
            s_ota_in_progress = false;
            ota_task_exit();
            return;
        }
    }

    // Extend the task WDT timeout for the flash phase. esp_flash_write +
    // cache_disable windows plus the consumer pause window can easily exceed
    // CONFIG_ESP_TASK_WDT_TIMEOUT_S, tripping WDT-subscribed tasks (idle
    // tasks, consumer mining tasks blocked in their pause primitives).
    // Restored on every exit path via ota_task_exit().
    ota_wdt_extend();

    // WiFi IP pre-flight — catches OTA attempts fired during reconnect
    // (stale DNS cache, no bound IP). One short retry then bail.
    if (!bb_wifi_has_ip()) {
        bb_log_w(TAG, "wifi has no IP — deferring OTA 2s and retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!bb_wifi_has_ip()) {
            ota_set_error("wifi not ready");
            goto resume_and_exit;
        }
    }

    // Acquire PM lock to prevent DVFS during flash operations.
    // This avoids the race between esp_cpu_unstall and get_rtc_dbias_by_efuse
    // observed on boards with live idle tasks on the other core.
    ota_pm_lock_acquire();

    bb_log_i(TAG, "ota worker on core %d", xPortGetCoreID());
    bb_log_i(TAG, "starting OTA update from %s", result.asset_url);
    taskENTER_CRITICAL(&s_ota_status_mux);
    s_ota_status.state = OTA_STATE_DOWNLOADING;
    s_ota_status.progress_pct = 0;
    s_ota_status.last_error[0] = '\0';
    taskEXIT_CRITICAL(&s_ota_status_mux);

    esp_http_client_config_t http_config = {
        .url = result.asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* WDT exclusion (see ota_worker_task entry) lifts the coupling between
         * this timeout and CONFIG_ESP_TASK_WDT_TIMEOUT_S. A stalled socket now
         * produces a clean HTTP error rather than a panic. Timeout is consumer-
         * tunable via bb_ota_pull_set_http_timeout_ms(); default 20 s. */
        .timeout_ms = (int)s_http_timeout_ms,
        .user_agent = esp_app_get_description()->project_name,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    // Verify OTA partition exists before attempting
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        bb_log_e(TAG, "no OTA update partition found");
        ota_set_error("no OTA update partition (check partition table)");
        goto resume_and_exit;
    }
    bb_log_i(TAG, "OTA target partition: %s", update_partition->label);

    // Retry handshake/header-read errors up to 3 times. TLS handshake flakes
    // (mbedtls fatal alert, ECONNRESET during open) and GitHub CDN redirect
    // hiccups are transient — a short backoff usually clears them. Don't retry
    // past get_img_desc: once we start writing flash we're committed.
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = ESP_FAIL;
    esp_app_desc_t img_desc;
    const int max_attempts = 3;
    const int backoff_ms[] = {5000, 15000, 30000};
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        err = esp_https_ota_begin(&ota_config, &ota_handle);
        if (err == ESP_OK) {
            err = esp_https_ota_get_img_desc(ota_handle, &img_desc);
            if (err == ESP_OK) {
                if (attempt > 0) {
                    bb_log_i(TAG, "OTA handshake succeeded on attempt %d/%d",
                             attempt + 1, max_attempts);
                }
                break;
            }
            bb_log_w(TAG, "ota_get_img_desc attempt %d/%d failed: %s",
                     attempt + 1, max_attempts, esp_err_to_name(err));
            esp_https_ota_abort(ota_handle);
            ota_handle = NULL;
        } else {
            bb_log_w(TAG, "ota_begin attempt %d/%d failed: %s",
                     attempt + 1, max_attempts, esp_err_to_name(err));
        }
        if (attempt + 1 < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
        }
    }
    if (err != ESP_OK) {
        bb_log_e(TAG, "OTA handshake failed after %d attempts: %s",
                 max_attempts, esp_err_to_name(err));
        ota_set_error("ota handshake failed after %d attempts: %s",
                      max_attempts, esp_err_to_name(err));
        goto resume_and_exit;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (strncmp(img_desc.project_name, running->project_name,
                sizeof(img_desc.project_name)) != 0) {
        // Check if consumer provided a skip-check callback
        bool skip_check = (s_skip_check_cb != NULL && s_skip_check_cb());
        if (skip_check) {
            bb_log_w(TAG, "OTA project-name mismatch, skipping check per consumer request: got '%s', expected '%s'",
                     img_desc.project_name, running->project_name);
        } else {
            bb_log_e(TAG, "board mismatch: got '%s', expected '%s'",
                     img_desc.project_name, running->project_name);
            ota_set_error("board mismatch: got '%s', expected '%s'",
                          img_desc.project_name, running->project_name);
            esp_https_ota_abort(ota_handle);
            goto resume_and_exit;
        }
    }

    int image_size = esp_https_ota_get_image_size(ota_handle);
    bb_log_i(TAG, "OTA download starting (image size: %d bytes)", image_size);

    int last_logged_pct = -1;
    int64_t last_progress_us = esp_timer_get_time();
    int last_read = 0;
    while (true) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int read_so_far = esp_https_ota_get_image_len_read(ota_handle);
        if (image_size > 0) {
            int pct = (read_so_far * 100) / image_size;
            taskENTER_CRITICAL(&s_ota_status_mux);
            s_ota_status.progress_pct = pct;
            taskEXIT_CRITICAL(&s_ota_status_mux);
            if (pct / 10 != last_logged_pct / 10) {
                bb_log_i(TAG, "OTA progress: %d%% (%d/%d bytes)",
                         pct, read_so_far, image_size);
                last_logged_pct = pct;
            }
        }
        // Warn if no bytes moved in the last 10s — surfaces a stalled
        // socket that would otherwise look like silence in the logs.
        int64_t now_us = esp_timer_get_time();
        if (read_so_far != last_read) {
            last_read = read_so_far;
            last_progress_us = now_us;
        } else if (now_us - last_progress_us > 10LL * 1000 * 1000) {
            bb_log_w(TAG, "OTA stalled: no bytes for >10s at %d bytes", read_so_far);
            last_progress_us = now_us;
        }
    }
    if (err != ESP_OK) {
        bb_log_e(TAG, "esp_https_ota_perform exited with: %s", esp_err_to_name(err));
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        bb_log_e(TAG, "incomplete OTA data received");
        ota_set_error("incomplete OTA data received");
        esp_https_ota_abort(ota_handle);
        goto resume_and_exit;
    }

    taskENTER_CRITICAL(&s_ota_status_mux);
    s_ota_status.state = OTA_STATE_VERIFYING;
    s_ota_status.progress_pct = 100;
    taskEXIT_CRITICAL(&s_ota_status_mux);

    err = esp_https_ota_finish(ota_handle);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_status.state = OTA_STATE_COMPLETE;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        bb_log_i(TAG, "OTA complete, rebooting to %s", result.latest_tag);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    bb_log_e(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
    ota_set_error("esp_https_ota_finish: %s", esp_err_to_name(err));

resume_and_exit:
    ota_pm_lock_release();
    s_ota_in_progress = false;
    if (paused && s_resume_cb) {
        s_resume_cb();
    }

    ota_task_exit();
}

/**
 * GET /api/ota/check - Kick the bb_update_check worker and return immediately.
 *
 * bb_update_check owns the single persistent 8 KB worker (s_worker). Delegating
 * here eliminates the duplicate per-call 12 KB ota_check_worker_task that
 * previously caused OOM under fragmented heap on bitaxe-650 (TA-378). Callers
 * poll GET /api/update/status for the result; the response shape is unchanged
 * so the webui does not need to change.
 */
static bb_err_t ota_check_handler(bb_http_request_t *req)
{
    bb_update_check_kick();  // truly non-blocking; ignore return (no URL set is OK)

    const char *response = "{\"status\":\"checking\"}";
    bb_http_resp_set_status(req, 200);
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send(req, response, strlen(response));
    return BB_OK;
}

/**
 * POST /api/ota/update - Trigger firmware update
 *
 * Reads bb_update_check's cached status instead of maintaining a parallel
 * cache. Returns 503 if no successful check has run yet, 409 if the check
 * reports no update available or an OTA is already in progress.
 */
static bb_err_t ota_update_handler(bb_http_request_t *req)
{
    // Atomically check and set s_ota_in_progress
    taskENTER_CRITICAL(&s_ota_status_mux);
    if (s_ota_in_progress) {
        taskEXIT_CRITICAL(&s_ota_status_mux);
        const char *response = "{\"error\":\"update_in_progress\"}";
        bb_http_resp_set_status(req, 409);
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send(req, response, strlen(response));
        return BB_OK;
    }
    s_ota_in_progress = true;
    taskEXIT_CRITICAL(&s_ota_status_mux);

    // Read bb_update_check's cached status — no second TLS handshake needed.
    bb_update_check_status_t uc_status;
    bb_err_t uc_err = bb_update_check_get_status(&uc_status);
    if (uc_err != BB_OK || !uc_status.last_check_ok) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        const char *response = "{\"error\":\"no_recent_check\"}";
        bb_http_resp_set_status(req, 503);
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send(req, response, strlen(response));
        return BB_OK;
    }

    if (!uc_status.available) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        const char *response = "{\"status\":\"already_up_to_date\"}";
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send(req, response, strlen(response));
        return BB_OK;
    }

    // Build a task argument from the cached check status.
    ota_worker_arg_t *task_arg = malloc(sizeof(ota_worker_arg_t));
    if (!task_arg) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        const char *response = "{\"error\":\"allocation_failed\"}";
        bb_http_resp_set_status(req, 500);
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send(req, response, strlen(response));
        return BB_OK;
    }

    strncpy(task_arg->latest_tag, uc_status.latest,       sizeof(task_arg->latest_tag) - 1);
    task_arg->latest_tag[sizeof(task_arg->latest_tag) - 1] = '\0';
    strncpy(task_arg->asset_url,  uc_status.download_url, sizeof(task_arg->asset_url) - 1);
    task_arg->asset_url[sizeof(task_arg->asset_url) - 1] = '\0';

    taskENTER_CRITICAL(&s_ota_status_mux);
    s_ota_status.state = OTA_STATE_CHECKING;
    s_ota_status.progress_pct = 0;
    s_ota_status.last_error[0] = '\0';
    taskEXIT_CRITICAL(&s_ota_status_mux);

    TaskHandle_t task_handle = NULL;
    BaseType_t task_result = xTaskCreatePinnedToCore(
        ota_worker_task,
        "ota_pull",
        OTA_TASK_STACK,
        task_arg,
        OTA_TASK_PRIO,
        &task_handle,
        s_ota_task_core
    );

    if (task_result != pdPASS) {
        free(task_arg);
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        const char *response = "{\"error\":\"task_create_failed\"}";
        bb_http_resp_set_status(req, 500);
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send(req, response, strlen(response));
        return BB_OK;
    }

    const char *response = "{\"status\":\"update_started\"}";
    bb_http_resp_set_status(req, 202);
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send(req, response, strlen(response));

    return BB_OK;
}

/**
 * GET /api/ota/status - Return OTA debug status
 */
static bb_err_t ota_status_handler(bb_http_request_t *req)
{
    // Snapshot status under lock
    ota_status_t status_copy;
    bool in_progress;
    taskENTER_CRITICAL(&s_ota_status_mux);
    memcpy(&status_copy, &s_ota_status, sizeof(status_copy));
    in_progress = s_ota_in_progress;
    taskEXIT_CRITICAL(&s_ota_status_mux);

    bb_json_t root = bb_json_obj_new();
    if (!root) {
        const char *response = "{\"error\":\"json_error\"}";
        bb_http_resp_set_status(req, 500);
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send(req, response, strlen(response));
        return BB_OK;
    }

    bb_json_obj_set_string(root, "state", s_ota_state_names[status_copy.state]);
    bb_json_obj_set_bool(root, "in_progress", in_progress);
    bb_json_obj_set_number(root, "progress_pct", status_copy.progress_pct);
    if (status_copy.last_error[0] != '\0') {
        bb_json_obj_set_string(root, "last_error", status_copy.last_error);
    }

    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

// ---------------------------------------------------------------------------
// Route descriptors (handler registered via raw httpd API; descriptors are
// descriptor-only entries added to the registry for OpenAPI spec emission)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_ota_check_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "check kicked; poll GET /api/update/status for result" },
    { 0 },
};

static const bb_route_t s_ota_check_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/ota/check",
    .tag      = "ota",
    .summary  = "Check for firmware update",
    .responses = s_ota_check_responses,
    .handler  = NULL,
};

static const bb_route_response_t s_ota_update_responses[] = {
    { 202, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "update download started" },
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "already up to date" },
    { 409, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "update already in progress or no update available" },
    { 503, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "no recent successful check — run GET /api/ota/check first" },
    { 0 },
};

static const bb_route_t s_ota_update_route = {
    .method   = BB_HTTP_POST,
    .path     = "/api/ota/update",
    .tag      = "ota",
    .summary  = "Trigger firmware update",
    .responses = s_ota_update_responses,
    .handler  = NULL,
};

static const bb_route_response_t s_ota_status_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"state\":{\"type\":\"string\","
      "\"enum\":[\"idle\",\"checking\",\"downloading\",\"verifying\",\"complete\",\"error\"]},"
      "\"in_progress\":{\"type\":\"boolean\"},"
      "\"progress_pct\":{\"type\":\"integer\"},"
      "\"last_error\":{\"type\":\"string\"}},"
      "\"required\":[\"state\",\"in_progress\",\"progress_pct\"]}",
      "OTA state machine status" },
    { 0 },
};

static const bb_route_t s_ota_status_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/ota/status",
    .tag      = "ota",
    .summary  = "Get OTA status",
    .responses = s_ota_status_responses,
    .handler  = NULL,
};

/**
 * Register OTA pull HTTP handlers with an existing httpd instance.
 */
static bb_err_t bb_ota_pull_init(bb_http_handle_t server)
{
    if (!server) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t err = bb_http_register_route(server, BB_HTTP_GET,
                                          "/api/ota/check", ota_check_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/ota/check handler");
        return err;
    }

    err = bb_http_register_route(server, BB_HTTP_POST,
                                 "/api/ota/update", ota_update_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/ota/update handler");
        return err;
    }

    err = bb_http_register_route(server, BB_HTTP_GET,
                                 "/api/ota/status", ota_status_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/ota/status handler");
        return err;
    }

    // Add descriptors to registry for OpenAPI spec emission.
    bb_err_t desc_err = bb_http_register_route_descriptor_only(&s_ota_check_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register ota-check descriptor: %d", desc_err);
    }
    desc_err = bb_http_register_route_descriptor_only(&s_ota_update_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register ota-update descriptor: %d", desc_err);
    }
    desc_err = bb_http_register_route_descriptor_only(&s_ota_status_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register ota-status descriptor: %d", desc_err);
    }

    bb_log_i(TAG, "OTA pull handlers registered");
    return BB_OK;
}

#if CONFIG_BB_OTA_PULL_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_ota_pull, bb_ota_pull_init, 3);
#endif

/**
 * Trigger an immediate OTA check (non-blocking).
 * Delegates to bb_update_check_now() — the single source of truth for
 * manifest fetches. bb_update_check owns the persistent 8 KB worker.
 */
bb_err_t bb_ota_pull_check_now(void)
{
    return bb_update_check_now();
}

void bb_ota_pull_set_task_core(int core)
{
    s_ota_task_core = core;
}

#endif // ESP_PLATFORM
