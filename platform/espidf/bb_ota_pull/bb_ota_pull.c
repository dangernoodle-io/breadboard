#include "bb_ota_pull.h"
#include "bb_update_check.h"
#include "bb_release_manifest.h"
#include "bb_http_client.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef ESP_PLATFORM
#include "bb_ota_pull_test_hooks.h"
#endif

#ifdef ESP_PLATFORM
#include "bb_http.h"
#include "bb_log.h"
#include "bb_registry.h"
#include "bb_system.h"
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
#endif

// Pluggable pause/resume callbacks
static bb_ota_pause_cb_t s_pause_cb = NULL;
static bb_ota_resume_cb_t s_resume_cb = NULL;

// Pluggable skip-check callback
static bb_ota_skip_check_cb_t s_skip_check_cb = NULL;

// Optional progress callback (LED/feedback) — shared bb_core typedef.
static bb_ota_progress_cb_t s_progress_cb = NULL;

// Releases URL — caller must set before bb_ota_pull_check_now()
// GitHub release-asset URLs are ~120-180 bytes; 256 is sufficient.
static char s_releases_url[256] = "";

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
uint32_t bb_ota_pull_host_get_http_timeout_ms(void)
{
    return s_http_timeout_ms;
}
#endif

#ifdef ESP_PLATFORM

static const char *TAG = "bb_ota_pull";

#define OTA_TASK_STACK 12288
#define OTA_TASK_PRIO  3

static volatile bool s_ota_in_progress = false;
static int s_ota_task_core = 1;  // default: Core 1 (bitaxe-friendly, frees Core 0 for httpd/stratum)
// Worker priority. On single-core targets the worker must outrank a CPU-bound
// consumer task (e.g. a SW-mining hot loop) so it can preempt and call the pause
// hook; otherwise the download starves the idle task and trips the WDT. Consumers
// raise this via bb_ota_pull_set_task_priority (mirrors bb_update_check).
static int s_ota_task_prio = OTA_TASK_PRIO;

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

// Public API: Set progress callback (LED/feedback during the pull)
void bb_ota_pull_set_progress_cb(bb_ota_progress_cb_t cb)
{
    s_progress_cb = cb;
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
 * matching is taken from bb_update_check_get_status() — the single source
 * of truth for the configured board name.
 *
 * Returns BB_OK, BB_ERR_INVALID_STATE (transport/HTTP error), or
 * BB_ERR_NOT_FOUND (parse: tag/asset missing).
 */
static bb_err_t ota_fetch_manifest(char *out_tag, size_t tag_cap,
                                   char *out_url, size_t url_cap)
{
    bb_update_check_status_t uc_status;
    bb_err_t status_err = bb_update_check_get_status(&uc_status);
    const char *board = (status_err == BB_OK && uc_status.board[0] != '\0')
                        ? uc_status.board : "unknown";

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
    // BB_OK with empty url means the release was parsed but no asset matched
    // this board. Treat as NOT_FOUND so callers can distinguish from a full match.
    if (out_url[0] == '\0') {
        return BB_ERR_NOT_FOUND;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Portable retry-decision helper — no ESP-IDF types; compiled on host+device.
// ---------------------------------------------------------------------------

/**
 * Returns true when an OTA download attempt should be retried.
 *
 * A download must be retried if the perform loop exited with an error OR the
 * complete-data flag was not set. Board-name mismatch is NOT passed here — it
 * is deterministic and handled before the perform loop.
 *
 * @param perform_err  last error code from esp_https_ota_perform()
 *                     (pass 0 / BB_OK when perform loop completed without error)
 * @param data_complete  result of esp_https_ota_is_complete_data_received()
 * @return true if the attempt failed and the caller should retry
 */
bool bb_ota_pull_download_should_retry(int perform_err, bool data_complete)
{
    return (perform_err != 0) || !data_complete;
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

// Wrappers: extend/restore WDT timeout around the OTA flash phase.
static void ota_wdt_extend(void)  { bb_system_wdt_set_timeout(CONFIG_BB_OTA_PULL_WDT_EXTENDED_S); }
static void ota_wdt_restore(void) { bb_system_wdt_set_timeout(CONFIG_ESP_TASK_WDT_TIMEOUT_S); }

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

// Fire the optional progress callback (LED/feedback). No-op if unset.
static void ota_progress(bb_ota_phase_t phase, int pct)
{
    bb_ota_progress_cb_t cb = s_progress_cb;
    if (cb) cb(phase, pct);
}

/**
 * ota_download_and_flash — generic download + flash core, runs on the CALLER's
 * task. Acquires the PM lock and extends the task WDT for the flash phase (both
 * restored on every exit). Does NOT pause/resume work, touch s_ota_in_progress,
 * manage task-WDT subscription, or reboot — the caller owns task lifecycle and
 * the post-success esp_restart(). Returns BB_OK when the new image is written
 * and ready to boot, else an error (status/last_error already set).
 *
 * Shared by ota_worker_task (spawn path) and bb_ota_pull_run_sync (boot-mode
 * full-heap path) so there is a single tested download path.
 */
static bb_err_t ota_download_and_flash(const char *asset_url)
{
    bb_err_t ret = ESP_FAIL;

    ota_wdt_extend();

    // WiFi IP pre-flight — catches OTA attempts fired during reconnect
    // (stale DNS cache, no bound IP). One short retry then bail.
    if (!bb_wifi_has_ip()) {
        bb_log_w(TAG, "wifi has no IP — deferring OTA 2s and retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!bb_wifi_has_ip()) {
            ota_set_error("wifi not ready");
            goto done;
        }
    }

    // Acquire PM lock to prevent DVFS during flash operations.
    // This avoids the race between esp_cpu_unstall and get_rtc_dbias_by_efuse
    // observed on boards with live idle tasks on the other core.
    ota_pm_lock_acquire();

    bb_log_i(TAG, "ota worker on core %d", xPortGetCoreID());
    bb_log_i(TAG, "starting OTA update from %s", asset_url);
    taskENTER_CRITICAL(&s_ota_status_mux);
    s_ota_status.state = OTA_STATE_DOWNLOADING;
    s_ota_status.progress_pct = 0;
    s_ota_status.last_error[0] = '\0';
    taskEXIT_CRITICAL(&s_ota_status_mux);
    ota_progress(BB_OTA_PHASE_START, 0);

    esp_http_client_config_t http_config = {
        .url = asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* WDT exclusion (see ota_worker_task entry) lifts the coupling between
         * this timeout and CONFIG_ESP_TASK_WDT_TIMEOUT_S. A stalled socket now
         * produces a clean HTTP error rather than a panic. Timeout is consumer-
         * tunable via bb_ota_pull_set_http_timeout_ms(); default 20 s. */
        .timeout_ms = (int)s_http_timeout_ms,
        .user_agent = esp_app_get_description()->project_name,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        /* Keep-alive is required for partial-http-download (Range requests on
         * the same connection). */
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        /* Chunk the firmware download via HTTP Range headers (4 KB per GET).
         * Without this, the server can send TLS records up to 16 KB and
         * mbedtls's IN buffer must be sized to match — that's the ~17 KB
         * alloc that fails on fragmented internal heap when no PSRAM is
         * available (tdongle-s3, esp32-wroom32). With chunked downloads each
         * TLS record is ~5 KB and consumers can compile-time lower
         * CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN to 8 KB, dropping the mbedtls
         * peak allocation from ~17 KB to ~9 KB. */
        .partial_http_download = true,
        .max_http_request_size = 4096,
    };

    // Verify OTA partition exists before attempting
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        bb_log_e(TAG, "no OTA update partition found");
        ota_set_error("no OTA update partition (check partition table)");
        goto done_release;
    }
    bb_log_i(TAG, "OTA target partition: %s", update_partition->label);

    // Download retry loop — covers begin → get_img_desc → board check →
    // perform loop → completeness check as a single retryable unit.
    //
    // Transient failures anywhere in the download phase (TLS handshake flakes,
    // CDN connection drops mid-transfer, partial reads) retry from scratch with
    // backoff. Board-name mismatch is deterministic and never retried.
    //
    // Each failed attempt aborts and NULLs the handle before sleeping so no
    // socket or handle leaks across retries.
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = ESP_FAIL;
    esp_app_desc_t img_desc;
    const int max_dl_attempts = 3;
    const int dl_backoff_ms[] = {3000, 10000, 20000};
    bool download_ok = false;

    for (int dl = 0; dl < max_dl_attempts && !download_ok; dl++) {
        if (dl > 0) {
            bb_log_i(TAG, "OTA download attempt %d/%d (backoff %d ms)",
                     dl + 1, max_dl_attempts, dl_backoff_ms[dl - 1]);
            vTaskDelay(pdMS_TO_TICKS(dl_backoff_ms[dl - 1]));
        }

        // --- begin + get_img_desc (kept as inner retry for handshake flakes) ---
        // Each inner attempt that opens a handle aborts it on failure so the
        // outer loop always starts with ota_handle == NULL.
        err = ESP_FAIL;
        for (int attempt = 0; attempt < max_dl_attempts; attempt++) {
            err = esp_https_ota_begin(&ota_config, &ota_handle);
            if (err == ESP_OK) {
                err = esp_https_ota_get_img_desc(ota_handle, &img_desc);
                if (err == ESP_OK) {
                    if (attempt > 0) {
                        bb_log_i(TAG, "OTA handshake succeeded on attempt %d/%d",
                                 attempt + 1, max_dl_attempts);
                    }
                    break;
                }
                bb_log_w(TAG, "ota_get_img_desc attempt %d/%d failed: %s",
                         attempt + 1, max_dl_attempts, esp_err_to_name(err));
                esp_https_ota_abort(ota_handle);
                ota_handle = NULL;
            } else {
                bb_log_w(TAG, "ota_begin attempt %d/%d failed: %s",
                         attempt + 1, max_dl_attempts, esp_err_to_name(err));
                ota_handle = NULL;
            }
            if (attempt + 1 < max_dl_attempts) {
                vTaskDelay(pdMS_TO_TICKS(dl_backoff_ms[attempt]));
            }
        }
        if (err != ESP_OK) {
            // Handshake exhausted inner retries — count this as one download
            // attempt and let the outer loop decide whether to retry.
            bb_log_w(TAG, "OTA download attempt %d/%d: handshake failed: %s",
                     dl + 1, max_dl_attempts, esp_err_to_name(err));
            // ota_handle is already NULL here (aborted in inner loop)
            continue;
        }

        // --- Board-name mismatch check (deterministic — do NOT retry) ---
        const esp_app_desc_t *running = esp_app_get_description();
        if (strncmp(img_desc.project_name, running->project_name,
                    sizeof(img_desc.project_name)) != 0) {
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
                ota_handle = NULL;
                goto done_release;
            }
        }

        // --- Perform loop ---
        int image_size = esp_https_ota_get_image_size(ota_handle);
        bb_log_i(TAG, "OTA download starting (image size: %d bytes)", image_size);

        // Reset per-attempt progress state so stall detection and progress
        // logging are sane on retried attempts.
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
                    ota_progress(BB_OTA_PHASE_PROGRESS, pct);
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

            // Yield each iteration so the IDLE task (and the WiFi/lwIP stack)
            // get CPU during the download+write loop. On single-core targets
            // the OTA worker otherwise monopolizes the core for the whole
            // transfer — with software AES the decrypt is CPU-bound and rarely
            // blocks on recv — and the task WDT trips the idle task; the
            // extended WDT timeout only delays that, it does not prevent it.
            // One tick per ~4 KB chunk is negligible overhead (a few seconds
            // across a 1 MB image) and also lets the TCP window refill,
            // improving throughput.
            vTaskDelay(1);
        }

        // --- Completeness check ---
        if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota_handle)) {
            const char *reason = (err != ESP_OK)
                ? esp_err_to_name(err) : "incomplete data received";
            bb_log_w(TAG, "OTA download attempt %d/%d failed: %s",
                     dl + 1, max_dl_attempts, reason);
            esp_https_ota_abort(ota_handle);
            ota_handle = NULL;
            // Outer loop will retry or exhaust attempts
            continue;
        }

        download_ok = true;
    }

    if (!download_ok) {
        bb_log_e(TAG, "OTA download failed after %d attempts", max_dl_attempts);
        ota_set_error("download failed after %d attempts", max_dl_attempts);
        // ota_handle is NULL here (aborted in last failed iteration)
        goto done_release;
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
        ret = BB_OK;        // image written; caller reboots
        goto done_release;
    }

    bb_log_e(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
    ota_set_error("esp_https_ota_finish: %s", esp_err_to_name(err));
    ret = err;

done_release:
    ota_pm_lock_release();
done:
    ota_wdt_restore();
    ota_progress(ret == BB_OK ? BB_OTA_PHASE_SUCCESS : BB_OTA_PHASE_FAIL,
                 ret == BB_OK ? 100 : 0);
    return ret;
}

/**
 * OTA worker task — spawn path for POST /api/update/apply. Removes itself from
 * the task WDT, pauses cooperative work, runs the shared download core, then
 * reboots on success or resumes work on failure.
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

    // Cooperatively pause work before the download. The pause window plus the
    // flash phase can exceed CONFIG_ESP_TASK_WDT_TIMEOUT_S; the core extends the
    // WDT (and ota_task_exit restores it).
    bool paused = (s_pause_cb != NULL) ? s_pause_cb() : false;
    bool pause_ok = (s_pause_cb == NULL) || paused;
    if (!pause_ok) {
        ota_set_error("mining pause failed — aborting OTA to avoid tls contention");
        s_ota_in_progress = false;
        ota_task_exit();
        return;
    }

    bb_err_t err = ota_download_and_flash(result.asset_url);
    if (err == BB_OK) {
        bb_log_i(TAG, "OTA complete, rebooting to %s", result.latest_tag);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    s_ota_in_progress = false;
    if (paused && s_resume_cb) {
        s_resume_cb();
    }
    ota_task_exit();
}

/**
 * bb_ota_pull_run_sync — run the firmware download+flash synchronously on the
 * CALLING task (no worker spawn). On BB_OK the new image is written and the
 * caller should reboot. Intended for the OTA-only boot-mode path where the full
 * heap is available because no subsystems have started. Does not pause/resume
 * (nothing is running) and does not reboot (caller decides).
 */
bb_err_t bb_ota_pull_run_sync(const char *asset_url)
{
    if (!asset_url || asset_url[0] == '\0') return ESP_ERR_INVALID_ARG;
    // The caller (boot task) is long-lived during the download; drop it from
    // the task WDT if subscribed (best-effort — the boot task usually isn't).
    esp_task_wdt_delete(NULL);
    return ota_download_and_flash(asset_url);
}

/**
 * POST /api/update/check - Kick the bb_update_check worker and return immediately.
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

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj, "status", "checking");
    return bb_http_resp_json_obj_end(&obj);
}

/**
 * POST /api/update/apply - Trigger firmware update
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
        bb_http_resp_set_status(req, 409);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "update_in_progress");
        bb_http_resp_json_obj_end(&obj);
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
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "no_recent_check");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    if (!uc_status.available) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "status", "already_up_to_date");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    // Build a task argument from the cached check status.
    ota_worker_arg_t *task_arg = malloc(sizeof(ota_worker_arg_t));
    if (!task_arg) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "allocation_failed");
        bb_http_resp_json_obj_end(&obj);
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

    // On single-core (unicore) targets, core 1 does not exist and
    // xTaskCreatePinnedToCore asserts; fall back to no affinity.
    int ota_task_core = s_ota_task_core;
    if (ota_task_core != tskNO_AFFINITY && ota_task_core >= configNUMBER_OF_CORES) {
        ota_task_core = tskNO_AFFINITY;
    }
    TaskHandle_t task_handle = NULL;
    BaseType_t task_result = xTaskCreatePinnedToCore(
        ota_worker_task,
        "ota_pull",
        OTA_TASK_STACK,
        task_arg,
        s_ota_task_prio,
        &task_handle,
        ota_task_core
    );

    if (task_result != pdPASS) {
        free(task_arg);
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "task_create_failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "update_started");
    return bb_http_resp_json_obj_end(&obj);
}

/**
 * GET /api/update/progress - Return OTA progress/debug status
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

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj,  "state",        s_ota_state_names[status_copy.state]);
    bb_http_resp_json_obj_set_bool(&obj, "in_progress",  in_progress);
    bb_http_resp_json_obj_set_int(&obj,  "progress_pct", (int64_t)status_copy.progress_pct);
    if (status_copy.last_error[0] != '\0') {
        bb_http_resp_json_obj_set_str(&obj, "last_error", status_copy.last_error);
    }
    return bb_http_resp_json_obj_end(&obj);
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
    .method   = BB_HTTP_POST,
    .path     = "/api/update/check",
    .tag      = "update",
    .summary  = "Kick update check; poll GET /api/update/status for result",
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
      "no recent successful check — run POST /api/update/check first" },
    { 0 },
};

static const bb_route_t s_ota_update_route = {
    .method   = BB_HTTP_POST,
    .path     = "/api/update/apply",
    .tag      = "update",
    .summary  = "Apply firmware update (download + flash)",
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
    .path     = "/api/update/progress",
    .tag      = "update",
    .summary  = "Get OTA download/flash progress",
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

    bb_err_t err = bb_http_register_route(server, BB_HTTP_POST,
                                          "/api/update/check", ota_check_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/update/check handler");
        return err;
    }

    err = bb_http_register_route(server, BB_HTTP_POST,
                                 "/api/update/apply", ota_update_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/update/apply handler");
        return err;
    }

    err = bb_http_register_route(server, BB_HTTP_GET,
                                 "/api/update/progress", ota_status_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/update/progress handler");
        return err;
    }

    // Add descriptors to registry for OpenAPI spec emission.
    bb_err_t desc_err = bb_http_register_route_descriptor_only(&s_ota_check_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register update-check descriptor: %d", desc_err);
    }
    desc_err = bb_http_register_route_descriptor_only(&s_ota_update_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register update-apply descriptor: %d", desc_err);
    }
    desc_err = bb_http_register_route_descriptor_only(&s_ota_status_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register update-progress descriptor: %d", desc_err);
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

void bb_ota_pull_set_task_priority(int priority)
{
    s_ota_task_prio = priority;
}

#endif // ESP_PLATFORM
