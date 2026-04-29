#include "bb_ota_pull.h"
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

#define OTA_TASK_STACK 16384
#define OTA_TASK_PRIO  3
/* 12 KB headroom for the TLS handshake + JSON parse worker. Original 32 KB was
 * conservative; the smaller stack lets task creation succeed on heap-pressed
 * boards (bitaxe) where no 32 KB contiguous free block remains after ASIC +
 * mDNS + log writer allocations. */
#define OTA_CHECK_STACK 12288
#define OTA_CHECK_PRIO 3
#define API_BUF_MAX    32768

static volatile bool s_ota_in_progress = false;

// Pre-allocated API response buffer. Allocated once at register-handler time
// while heap is still mostly contiguous; reused on every check. Avoids the
// failure mode where the largest free contiguous block has fragmented below
// API_BUF_MAX after long uptime, causing on-demand malloc to fail and the
// background check to abort with "failed to allocate response buffer".
static char *s_api_buf = NULL;

static char *ota_pull_get_api_buf(void)
{
    if (s_api_buf) return s_api_buf;
    s_api_buf = malloc(API_BUF_MAX);
    if (!s_api_buf) {
        bb_log_e(TAG, "failed to allocate API buffer (%d bytes)", API_BUF_MAX);
    }
    return s_api_buf;
}
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
    bool update_available;
} ota_pull_check_result_t;

static bool s_check_in_progress = false;
static bool s_check_done = false;
static bool s_check_failed = false;
static ota_pull_check_result_t s_cached_check = {0};

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

/**
 * Targeted scanner for GitHub releases/latest JSON. Extracts only the two
 * fields we use (tag_name + matching asset's browser_download_url) without
 * loading the document into a parse tree. Removes the dependency on response
 * size: avoids the 16 KB cJSON_Parse cliff that has bricked OTA on devices
 * once auto-generated changelogs grew past the read buffer.
 *
 * Platform-independent, testable on host. Helpers are escape-aware (\" \\).
 */

/* Find the next `"<key>"<ws>:` pair in [p, end). Returns pointer to the
 * first non-whitespace byte of the value, or NULL if not found. Skips over
 * the contents of any string it encounters so it never matches a key
 * pattern that lives inside a string value. */
static const char *find_key(const char *p, const char *end, const char *key)
{
    size_t key_len = strlen(key);
    while (p < end) {
        if (*p == '"') {
            const char *str_start = ++p;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p += 2;
                else p++;
            }
            if (p >= end) return NULL;
            const char *str_end = p;  /* points at closing " */
            p++;
            const char *q = p;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
            if (q < end && *q == ':') {
                if ((size_t)(str_end - str_start) == key_len &&
                    memcmp(str_start, key, key_len) == 0) {
                    q++;
                    while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
                    return q;
                }
            }
        } else {
            p++;
        }
    }
    return NULL;
}

/* Copy a JSON string value into out (truncated to out_size-1, always null-
 * terminated). p must point at the opening `"`. Returns pointer past the
 * closing `"`, or NULL if not a string / unterminated. */
static const char *copy_string_value(const char *p, const char *end,
                                     char *out, size_t out_size)
{
    if (p >= end || *p != '"' || out_size == 0) return NULL;
    p++;
    size_t i = 0;
    while (p < end && *p != '"') {
        char c = *p;
        if (c == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
                case 'n':  c = '\n'; p++; break;
                case 't':  c = '\t'; p++; break;
                case 'r':  c = '\r'; p++; break;
                case 'b':  c = '\b'; p++; break;
                case 'f':  c = '\f'; p++; break;
                case '"':  c = '"';  p++; break;
                case '\\': c = '\\'; p++; break;
                case '/':  c = '/';  p++; break;
                case 'u':
                    /* \uXXXX — not present in tag_name or download URL; skip. */
                    if (p + 4 < end) p += 5;
                    else p = end;
                    continue;
                default:   c = *p;  p++; break;
            }
        } else {
            p++;
        }
        if (i + 1 < out_size) out[i++] = c;
    }
    if (p >= end) return NULL;  /* unterminated */
    out[i] = '\0';
    return p + 1;
}

/* p must point at '{'. Returns pointer to the matching '}', or NULL on
 * unbalanced / EOF. Tracks string boundaries so braces inside strings
 * don't confuse the depth count. */
static const char *match_brace(const char *p, const char *end)
{
    if (p >= end || *p != '{') return NULL;
    int depth = 1;
    p++;
    while (p < end && depth > 0) {
        if (*p == '"') {
            p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p += 2;
                else p++;
            }
            if (p >= end) return NULL;
            p++;
        } else if (*p == '{') {
            depth++;
            p++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) return p;
            p++;
        } else {
            p++;
        }
    }
    return NULL;
}

int bb_ota_pull_parse_release_json(const char *json, const char *board_name,
                                     char *out_tag, size_t tag_size,
                                     char *out_url, size_t url_size)
{
    if (!json || !board_name || !out_tag || !out_url) return -1;
    if (tag_size == 0 || url_size == 0) return -1;

    const char *end = json + strlen(json);

    /* tag_name is required; missing -> -1 (malformed). */
    const char *tag_p = find_key(json, end, "tag_name");
    if (!tag_p || *tag_p != '"') return -1;
    if (!copy_string_value(tag_p, end, out_tag, tag_size)) return -1;

    /* assets array: must be present AND an array. */
    const char *assets_p = find_key(json, end, "assets");
    if (!assets_p || *assets_p != '[') return -2;
    assets_p++;

    char asset_name[128];
    snprintf(asset_name, sizeof(asset_name), "%s.bin", board_name);

    const char *p = assets_p;
    while (p < end) {
        while (p < end && (*p == ',' || *p == ' ' || *p == '\t' ||
                           *p == '\n' || *p == '\r')) p++;
        if (p >= end || *p == ']') break;
        if (*p != '{') break;

        const char *obj_end = match_brace(p, end);
        if (!obj_end) return -1;

        char this_name[128] = {0};
        const char *name_p = find_key(p, obj_end, "name");
        if (name_p && *name_p == '"') {
            copy_string_value(name_p, obj_end, this_name, sizeof(this_name));
        }

        if (strcmp(this_name, asset_name) == 0) {
            const char *url_p = find_key(p, obj_end, "browser_download_url");
            if (!url_p || *url_p != '"') return -2;
            if (!copy_string_value(url_p, obj_end, out_url, url_size)) return -2;
            return 0;
        }

        p = obj_end + 1;
    }

    return -2;
}

#ifdef ESP_PLATFORM

static esp_err_t ota_pull_check(ota_pull_check_result_t *result)
{
    char *buf = ota_pull_get_api_buf();
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = s_releases_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = esp_app_get_description()->project_name,
        .buffer_size = 4096,
    };

    // Retry the open+headers handshake up to 3 times on transport errors.
    // TLS handshake flakes + GitHub CDN hiccups are common; the whole client
    // state is dirty after a failed open, so we reinit per attempt.
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = 0;
    int content_length = 0;
    const int max_attempts = 3;
    const int backoff_ms[] = {2000, 4000};
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        client = esp_http_client_init(&config);
        if (!client) {
            err = ESP_FAIL;
            bb_log_w(TAG, "http init attempt %d/%d failed",
                     attempt + 1, max_attempts);
        } else {
            esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
            esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");
            esp_http_client_set_method(client, HTTP_METHOD_GET);

            err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                content_length = esp_http_client_fetch_headers(client);
                status = esp_http_client_get_status_code(client);
                if (status == 200) {
                    if (attempt > 0) {
                        bb_log_i(TAG, "http open succeeded on attempt %d/%d",
                                 attempt + 1, max_attempts);
                    }
                    break;
                }
                bb_log_w(TAG, "http open attempt %d/%d: GitHub API returned %d",
                         attempt + 1, max_attempts, status);
                err = ESP_FAIL;
            } else {
                bb_log_w(TAG, "http open attempt %d/%d failed: %s",
                         attempt + 1, max_attempts, esp_err_to_name(err));
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
        }
        if (attempt + 1 < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
        }
    }
    if (err != ESP_OK || !client) {
        bb_log_e(TAG, "http open failed after %d attempts", max_attempts);
        goto cleanup;
    }

    int total = 0;
    int read_len;
    while (total < API_BUF_MAX - 1 &&
           (read_len = esp_http_client_read(client, buf + total,
                                            API_BUF_MAX - total - 1)) > 0) {
        total += read_len;
    }
    buf[total] = '\0';
    (void)content_length;

    if (total == 0) {
        bb_log_e(TAG, "empty response from GitHub API");
        err = ESP_FAIL;
        goto cleanup;
    }

    // Determine board name to use for asset lookup
    const char *board = s_firmware_board[0] != '\0' ? s_firmware_board : "unknown";

    int parse_ret = bb_ota_pull_parse_release_json(
        buf, board,
        result->latest_tag, sizeof(result->latest_tag),
        result->asset_url, sizeof(result->asset_url));

    if (parse_ret != 0) {
        bb_log_e(TAG, "failed to parse release json: %d", parse_ret);
        err = ESP_FAIL;
        goto cleanup;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    result->update_available = strncmp(running->version, "dev", 3) == 0 ||
                               strcmp(result->latest_tag, running->version) != 0;

    if (result->update_available) {
        bb_log_i(TAG, "update available: %s -> %s",
                 running->version, result->latest_tag);
    } else {
        bb_log_i(TAG, "already up to date: %s", result->latest_tag);
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    /* buf is the persistent s_api_buf — do not free */
    return err;
}

/**
 * Re-subscribe to the task WDT and delete the calling task.
 * Called at every exit path of worker tasks that removed themselves from
 * the WDT at entry. Best-effort: error is ignored — the task is dying.
 */
static void ota_task_exit(void)
{
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

    ota_pull_check_result_t result;
    if (arg) {
        memcpy(&result, arg, sizeof(ota_pull_check_result_t));
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
 * OTA check worker task - performs the version check in background.
 */
static void ota_check_worker_task(void *arg)
{
    // Remove this task from the task WDT for the same reason as ota_worker_task:
    // the TLS check involves blocking mbedtls recv that must not trip the WDT.
    esp_err_t wdt_err = esp_task_wdt_delete(NULL);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_NOT_FOUND) {
        bb_log_w(TAG, "esp_task_wdt_delete: %s", esp_err_to_name(wdt_err));
    }

    bb_log_i(TAG, "ota check worker on core %d", xPortGetCoreID());

    // Cooperatively pause work to free memory for TLS handshake
    bool paused = false;
    if (s_pause_cb) {
        paused = s_pause_cb();
    }

    // Acquire PM lock to prevent DVFS during HTTP/TLS operations
    ota_pm_lock_acquire();

    ota_pull_check_result_t result = {0};
    esp_err_t err = ota_pull_check(&result);

    // Release PM lock after the check is complete
    ota_pm_lock_release();

    bool ok = (err == ESP_OK);
    taskENTER_CRITICAL(&s_ota_status_mux);
    if (ok) {
        memcpy(&s_cached_check, &result, sizeof(ota_pull_check_result_t));
        s_check_done = true;
        s_check_failed = false;
    } else {
        s_check_done = false;
        s_check_failed = true;
    }
    s_check_in_progress = false;
    taskEXIT_CRITICAL(&s_ota_status_mux);

    if (ok) {
        bb_log_i(TAG, "background check completed");
    } else {
        bb_log_e(TAG, "background check failed");
    }

    // Resume mining after the "completed" log so the serial output reads in
    // the order operators expect (paused → result → completed → resumed).
    if (paused && s_resume_cb) {
        s_resume_cb();
    }

    ota_task_exit();
}

/**
 * GET /api/ota/check - Check for available updates (non-blocking)
 */
static bb_err_t ota_check_handler(bb_http_request_t *req)
{
    // Snapshot state under critical section. When the previous background
    // check failed, clear the sticky failure flag and fall through to the
    // retrigger path so the CLI can recover without the device needing a
    // reboot.
    bool check_done, check_in_progress;
    ota_pull_check_result_t cached;
    taskENTER_CRITICAL(&s_ota_status_mux);
    check_done = s_check_done;
    check_in_progress = s_check_in_progress;
    if (s_check_failed) {
        s_check_failed = false;
    }
    if (check_done) {
        memcpy(&cached, &s_cached_check, sizeof(cached));
        s_check_done = false;  // invalidate cache
    }
    taskEXIT_CRITICAL(&s_ota_status_mux);

    // If we have a cached result, return it
    if (check_done) {
        bb_json_t root = bb_json_obj_new();
        if (!root) {
            const char *error_response = "{\"error\":\"json_error\"}";
            bb_http_resp_set_status(req, 500);
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send(req, error_response, strlen(error_response));
            return BB_OK;
        }

        const esp_app_desc_t *running_desc = esp_app_get_description();
        if (running_desc) {
            bb_json_obj_set_string(root, "current_version", running_desc->version);
        }
        bb_json_obj_set_string(root, "latest_version", cached.latest_tag);
        bb_json_obj_set_bool(root, "update_available", cached.update_available);

        char asset_name[128];
        const char *board = s_firmware_board[0] != '\0' ? s_firmware_board : "unknown";
        snprintf(asset_name, sizeof(asset_name), "%s.bin", board);
        bb_json_obj_set_string(root, "asset", asset_name);

        char *response_str = bb_json_serialize(root);
        bb_json_free(root);

        if (response_str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send(req, response_str, strlen(response_str));
            bb_json_free_str(response_str);
        } else {
            const char *error_response = "{\"error\":\"json_error\"}";
            bb_http_resp_set_status(req, 500);
            bb_http_resp_send(req, error_response, strlen(error_response));
        }

        return BB_OK;
    }

    // Trigger background check if not already running
    if (!check_in_progress) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_check_in_progress = true;
        taskEXIT_CRITICAL(&s_ota_status_mux);

        BaseType_t task_result = xTaskCreatePinnedToCore(
            ota_check_worker_task,
            "ota_chk",
            OTA_CHECK_STACK,
            NULL,
            OTA_CHECK_PRIO,
            NULL,
            s_ota_task_core
        );

        if (task_result != pdPASS) {
            taskENTER_CRITICAL(&s_ota_status_mux);
            s_check_in_progress = false;
            taskEXIT_CRITICAL(&s_ota_status_mux);
            const char *response = "{\"error\":\"task_create_failed\"}";
            bb_http_resp_set_status(req, 500);
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send(req, response, strlen(response));
            return BB_OK;
        }
    }

    const char *response = "{\"status\":\"checking\"}";
    bb_http_resp_set_status(req, 202);
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send(req, response, strlen(response));
    return BB_OK;
}

/**
 * POST /api/ota/update - Trigger firmware update
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

    // Use cached check result instead of hitting GitHub again — avoids a second
    // TLS handshake that fails under memory pressure when work is active.
    ota_pull_check_result_t result = {0};
    taskENTER_CRITICAL(&s_ota_status_mux);
    memcpy(&result, &s_cached_check, sizeof(result));
    taskEXIT_CRITICAL(&s_ota_status_mux);

    if (!result.update_available) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        const char *response = "{\"status\":\"already_up_to_date\"}";
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send(req, response, strlen(response));
        return BB_OK;
    }

    // Allocate task argument
    ota_pull_check_result_t *task_arg = malloc(sizeof(ota_pull_check_result_t));
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

    memcpy(task_arg, &result, sizeof(ota_pull_check_result_t));
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

    char *response_str = bb_json_serialize(root);
    bb_json_free(root);

    if (response_str) {
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send(req, response_str, strlen(response_str));
        bb_json_free_str(response_str);
    } else {
        const char *response = "{\"error\":\"json_error\"}";
        bb_http_resp_set_status(req, 500);
        bb_http_resp_send(req, response, strlen(response));
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptors (handler registered via raw httpd API; descriptors are
// descriptor-only entries added to the registry for OpenAPI spec emission)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_ota_check_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"current_version\":{\"type\":\"string\"},"
      "\"latest_version\":{\"type\":\"string\"},"
      "\"update_available\":{\"type\":\"boolean\"},"
      "\"asset\":{\"type\":\"string\"}},"
      "\"required\":[\"latest_version\",\"update_available\"]}",
      "cached check result with version comparison" },
    { 202, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "check triggered in background" },
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
      "update already in progress" },
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

    /* Pre-allocate the API response buffer now, while heap is mostly contiguous.
     * Lazy allocation in ota_pull_check() fails on long-uptime boards (esp.
     * bitaxe with ASIC + mDNS + log writer task stacks fragmenting heap below
     * the 16 KB threshold). Failure here is non-fatal — check path will retry. */
    (void)ota_pull_get_api_buf();

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
    bb_http_register_route_descriptor_only(&s_ota_check_route);
    bb_http_register_route_descriptor_only(&s_ota_update_route);
    bb_http_register_route_descriptor_only(&s_ota_status_route);

    bb_log_i(TAG, "OTA pull handlers registered");
    return BB_OK;
}

#if CONFIG_BB_OTA_PULL_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_ota_pull, bb_ota_pull_init, 3);
#endif

/**
 * Trigger an immediate OTA check (non-blocking).
 */
bb_err_t bb_ota_pull_check_now(void)
{
    if (s_releases_url[0] == '\0') {
        bb_log_e(TAG, "releases URL not set; call bb_ota_pull_set_releases_url() first");
        return ESP_ERR_INVALID_STATE;
    }

    bool check_in_progress;
    taskENTER_CRITICAL(&s_ota_status_mux);
    check_in_progress = s_check_in_progress;
    s_check_in_progress = true;
    taskEXIT_CRITICAL(&s_ota_status_mux);

    if (check_in_progress) {
        return ESP_ERR_INVALID_STATE;  // Already checking
    }

    BaseType_t task_result = xTaskCreatePinnedToCore(
        ota_check_worker_task,
        "ota_chk",
        OTA_CHECK_STACK,
        NULL,
        OTA_CHECK_PRIO,
        NULL,
        s_ota_task_core
    );

    if (task_result != pdPASS) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_check_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void bb_ota_pull_set_task_core(int core)
{
    s_ota_task_core = core;
}

#endif // ESP_PLATFORM
