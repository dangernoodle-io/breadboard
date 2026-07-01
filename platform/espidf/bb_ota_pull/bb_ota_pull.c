#include "bb_ota_pull.h"
#include "bb_tls.h"
#include "bb_ota_hooks.h"
#include "bb_update_check.h"
#include "bb_release_manifest.h"
#include "bb_http_client.h"
#include <inttypes.h>
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
#include "bb_mem.h"
#include "bb_init.h"
#include "bb_wifi.h"
#include "bb_wdt.h"
#include "bb_board.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#endif

// Releases URL — caller must set before bb_ota_pull_check_now()
// GitHub release-asset URLs are ~120-180 bytes; 256 is sufficient.
static char s_releases_url[256] = "";

// Per-recv HTTP timeout for OTA download (ms). Consumer-tunable via
// bb_ota_pull_set_http_timeout_ms(). Default driven by Kconfig; fallback for
// host builds where CONFIG_* is undefined; pass 0 to restore the default.
#ifndef CONFIG_BB_OTA_PULL_HTTP_TIMEOUT_MS
#define CONFIG_BB_OTA_PULL_HTTP_TIMEOUT_MS 20000
#endif
#define BB_OTA_HTTP_TIMEOUT_MS_DEFAULT CONFIG_BB_OTA_PULL_HTTP_TIMEOUT_MS
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

// OTA chunk must fit inside a single TLS record; the TLS input buffer is
// CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN bytes.  Violating this causes an esp-http
// mbedTLS record-too-large error mid-download (observed as -0x7200).
_Static_assert(CONFIG_BB_OTA_PULL_HTTP_CHUNK_SIZE <= CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
    "BB_OTA_PULL_HTTP_CHUNK_SIZE must be <= MBEDTLS_SSL_IN_CONTENT_LEN "
    "(lower CONFIG_BB_OTA_PULL_HTTP_CHUNK_SIZE or raise "
    "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN)");

static const char *TAG = "bb_ota_pull";

#define OTA_TASK_STACK CONFIG_BB_OTA_PULL_TASK_STACK
#define OTA_TASK_PRIO  3

// Freshness window for the apply handler cache-first logic.
// Kconfig default 300 s; 0 = always refresh.
#ifndef CONFIG_BB_OTA_PULL_APPLY_CACHE_FRESH_S
#define CONFIG_BB_OTA_PULL_APPLY_CACHE_FRESH_S 300
#endif

// Max wait (ms) for the IP stack to settle before the first OTA attempt.
// Kconfig default 8000; 0 = no settle wait (fail fast if no IP).
#ifndef CONFIG_BB_OTA_PULL_WIFI_SETTLE_MS
#define CONFIG_BB_OTA_PULL_WIFI_SETTLE_MS 8000
#endif

// Timeout (ms) for bb_update_check_run_blocking() inside the apply handler.
// Must be long enough for a cold TLS handshake + manifest fetch under load;
// 15 s was too tight and caused spurious refresh-failed 503s on congested links.
#define BB_OTA_APPLY_REFRESH_TIMEOUT_MS 30000

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

// Forward declaration — defined in the portable section below.
const char *bb_ota_pull_resolve_redirect_url(const char *original_url,
                                             const char *resolved_url,
                                             int perform_err,
                                             bool *out_did_redirect);

/**
 * Pre-resolve a GitHub asset URL through any HTTP redirect before handing
 * it to esp_https_ota.
 *
 * GitHub release asset URLs (github.com/.../releases/download/...) 302-redirect
 * to a CDN (objects.githubusercontent.com). The chunked Range-request path used
 * by partial_http_download does not reliably follow cross-host redirects, so the
 * download fails after the retry budget is exhausted. Pre-resolving the URL here
 * (immediately before the download) hands esp_https_ota the final CDN URL,
 * bypassing the redirect entirely.
 *
 * Uses a short-lived HEAD request with auto-redirect enabled.  If GitHub does not
 * 302 on HEAD, a Range: bytes=0-0 GET is tried as a fallback.  On any failure the
 * original URL is returned so the caller degrades gracefully — the probe must
 * never make things worse.
 *
 * The CDN signed URL is time-limited (~5 min); this probe runs immediately before
 * the download, so the URL is fresh.  Do NOT cache or reuse the result.
 *
 * @param original_url   the GitHub asset URL to probe (must not be NULL)
 * @param out_buf        caller-supplied buffer for the resolved URL
 * @param out_buf_size   size of out_buf
 * @return the URL to use: out_buf when a redirect was followed, original_url otherwise
 */
static const char *ota_resolve_redirect(const char *original_url,
                                        char *out_buf, size_t out_buf_size)
{
    esp_http_client_config_t cfg = {
        .url                 = original_url,
        .method              = HTTP_METHOD_HEAD,
        .crt_bundle_attach   = esp_crt_bundle_attach,
        .timeout_ms          = (int)s_http_timeout_ms,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        bb_log_d(TAG, "redirect probe: client init failed, using original URL");
        return original_url;
    }

    esp_err_t err = esp_http_client_perform(client);

    // HEAD may return a non-200 status but still have followed the redirect.
    // Some CDNs reject HEAD with 403; fall back to a single-byte GET in that case.
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 403 || status == 405) {
            bb_log_d(TAG, "redirect probe: HEAD returned %d, retrying with Range GET", status);
            esp_http_client_set_method(client, HTTP_METHOD_GET);
            esp_http_client_set_header(client, "Range", "bytes=0-0");
            err = esp_http_client_perform(client);
        }
    }

    // esp_http_client_get_url copies the resolved URL into our buffer.
    // On failure or overflow it returns an error; treat as no-redirect.
    const char *resolved = NULL;
    if (err == ESP_OK) {
        esp_err_t url_err = esp_http_client_get_url(client, out_buf, (int)out_buf_size);
        if (url_err == ESP_OK) {
            resolved = out_buf;
        } else {
            bb_log_d(TAG, "redirect probe: get_url failed (%s), using original URL",
                     esp_err_to_name(url_err));
        }
    }

    bool did_redirect = false;
    const char *use_url = bb_ota_pull_resolve_redirect_url(original_url, resolved,
                                                           (int)err, &did_redirect);

    if (did_redirect) {
        bb_log_d(TAG, "redirect probe: resolved to CDN URL");
        esp_http_client_cleanup(client);
        return out_buf;
    }

    bb_log_d(TAG, "redirect probe: no redirect detected, using original URL");
    esp_http_client_cleanup(client);
    return original_url;
}

typedef struct {
    char latest_tag[32];
    char asset_url[256];
} ota_worker_arg_t;

#if CONFIG_BB_OTA_STATIC_STACK && CONFIG_BB_OTA_PULL_AUTOREGISTER
static StaticTask_t  s_ota_pull_task_buf;
static StackType_t   s_ota_pull_stack[OTA_TASK_STACK / sizeof(StackType_t)];
static ota_worker_arg_t s_ota_worker_arg;
#endif

#endif // ESP_PLATFORM

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

/**
 * Pure freshness predicate for the POST /api/update/apply cache-first logic.
 *
 * Returns true when the cached update-check result should be trusted without
 * a refresh:
 *   - last_check_ok must be true (a successful check ran)
 *   - the check must be younger than window_s seconds
 *   - window_s == 0 always returns false (always refresh)
 *
 * No ESP-IDF dependencies; exercised on host by the unit tests.
 */
bool bb_ota_pull_apply_cache_is_fresh(bool last_check_ok, int64_t last_check_us,
                                      int64_t now_us, int32_t window_s)
{
    if (window_s <= 0) return false;
    if (!last_check_ok) return false;
    if (last_check_us <= 0) return false;
    int64_t age_us = now_us - last_check_us;
    int64_t window_us = (int64_t)window_s * 1000000LL;
    return age_us <= window_us;
}

/**
 * Pure pre-flight heap guard predicate — no ESP-IDF dependencies.
 *
 * Returns true when the guard PASSES (OTA may proceed), false when either
 * heap dimension is below its configured floor.
 *
 * @param largest_block   heap_caps_get_largest_free_block() result (bytes)
 * @param contiguous_floor  minimum required contiguous block (0 = disabled)
 * @param total_free      heap_caps_get_free_size() result (bytes)
 * @param total_floor     minimum required total free heap (0 = disabled)
 * @param out_dim         on failure, set to "contiguous" or "total-free";
 *                        may be NULL if the caller does not need the label
 */

/**
 * Pure redirect-URL decision helper — no ESP-IDF dependencies.
 *
 * Given the original asset URL, the resolved URL returned by the redirect-probe
 * HTTP client, and the perform result, decides which URL to use for the OTA
 * download and whether a redirect was followed.
 *
 * Rules:
 *   - If perform_err != 0 (probe failed) → use original; did_redirect = false.
 *   - If resolved_url is NULL or empty → use original; did_redirect = false.
 *   - If resolved_url equals original_url → no redirect; use original.
 *   - Otherwise → redirect was followed; use resolved_url.
 *
 * Never returns NULL: always returns either original_url or resolved_url.
 *
 * @param original_url   the initial asset URL (must not be NULL)
 * @param resolved_url   URL after HTTP client performed (may be NULL)
 * @param perform_err    0 on success, non-zero on failure
 * @param out_did_redirect  set to true when a cross-host redirect was followed
 * @return the URL to use for the OTA download
 */
const char *bb_ota_pull_resolve_redirect_url(const char *original_url,
                                             const char *resolved_url,
                                             int perform_err,
                                             bool *out_did_redirect)
{
    bool did = false;
    const char *use = original_url;
    if (perform_err == 0 && resolved_url != NULL && resolved_url[0] != '\0') {
        if (strcmp(original_url, resolved_url) != 0) {
            use = resolved_url;
            did = true;
        }
    }
    if (out_did_redirect) *out_did_redirect = did;
    return use;
}

#ifdef BB_OTA_PULL_TESTING
// Test hook: run ota_fetch_manifest with the currently configured URL/board.
bb_err_t bb_ota_pull_fetch_manifest_for_test(char *out_tag, size_t tag_cap,
                                             char *out_url, size_t url_cap)
{
    return ota_fetch_manifest(out_tag, tag_cap, out_url, url_cap);
}

// Test hook: exercise the bb_ota_pull_heap_ready guard with synthetic heap
// values and caller-supplied floor constants (the real values are
// ESP-IDF-only; this lets host tests verify the predicate boundary).
bool bb_ota_pull_heap_ready_for_test(size_t largest_block, size_t contiguous_floor,
                                     size_t total_free, size_t total_floor)
{
    return bb_tls_heap_guard_passes(largest_block, contiguous_floor,
                                    total_free, total_floor, NULL);
}
#endif

#ifdef ESP_PLATFORM

/**
 * Restore the WDT timeout and delete the calling task.
 * Called at every exit path of worker tasks that removed themselves from the
 * WDT at entry. The task must NOT re-subscribe before dying: re-adding a task
 * that is about to be deleted leaves a dangling WDT entry pointing at a freed
 * TCB, which the next WDT check reads as a garbled task name that never feeds —
 * tripping a spurious task_wdt panic. The task stays unsubscribed as it exits.
 */
static void ota_task_exit(void)
{
    bb_update_check_ota_claim_release("ota_pull");
    bb_wdt_extend_end();
    vTaskDelete(NULL);
}

// Exponential download retry backoff: base, base*2, ... capped at max.
static uint32_t ota_dl_backoff_ms(int attempt)
{
    uint32_t b = (uint32_t)CONFIG_BB_OTA_PULL_DL_BACKOFF_BASE_MS;
    uint32_t cap = (uint32_t)CONFIG_BB_OTA_PULL_DL_BACKOFF_MAX_MS;
    for (int i = 0; i < attempt && b < cap; i++) b <<= 1;
    return b > cap ? cap : b;
}

// B1-358: extract hostname from a URL ("https://host/path" → "host").
// Writes into buf (NUL-terminates). Returns buf, or "?" on parse failure.
static const char *ota_url_host(const char *url, char *buf, size_t buf_len)
{
    if (!url || !buf || buf_len == 0) return "?";
    // skip scheme ("https://", "http://")
    const char *p = url;
    const char *s = strstr(p, "://");
    if (s) p = s + 3;
    // copy until '/', ':', '?', '#', or end
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (i + 1 < buf_len) buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return (i > 0) ? buf : "?";
}

// Pre-flight contiguous-heap floor for the OTA TLS handshake. Derives from
// the shared BB_TLS_HEAP_CONTIGUOUS_FLOOR knob (bridged in bb_tls.h):
//   0 (default) = auto-derive: BB_TLS_SSL_IN_FLOOR (SSL_IN + 1024)
//  >0            = explicit byte override
//  <0            = guard disabled
#if BB_TLS_HEAP_CONTIGUOUS_FLOOR > 0
#  define BB_OTA_HEAP_FLOOR_BYTES ((size_t)BB_TLS_HEAP_CONTIGUOUS_FLOOR)
#elif BB_TLS_HEAP_CONTIGUOUS_FLOOR < 0
#  define BB_OTA_HEAP_FLOOR_BYTES 0   /* guard disabled */
#else
#  define BB_OTA_HEAP_FLOOR_BYTES BB_TLS_SSL_IN_FLOOR
#endif

bool bb_ota_pull_heap_ready(void)
{
    size_t largest    = bb_board_heap_internal_largest_free_block();
    size_t total_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return bb_tls_heap_guard_passes(largest, BB_OTA_HEAP_FLOOR_BYTES,
                                    total_free, BB_TLS_HEAP_TOTAL_FLOOR,
                                    NULL);
}

#if BB_OTA_HEAP_FLOOR_BYTES > 0 || BB_TLS_HEAP_TOTAL_FLOOR > 0
/*
 * ota_heap_guard — sample both internal-heap dimensions and refuse cleanly if
 * either is below floor, logging a clear reason and setting last_error.
 *
 * Best-effort: the heap can fragment between any two samples under concurrent
 * httpd/SSE/wifi load, so this runs twice — "pre-flight" at entry and
 * "pre-handshake" with the PM lock held — to narrow, not close, the race
 * before esp_https_ota_begin().
 *   Dim-1 (contiguous): largest free block must hold the mbedTLS IN buffer.
 *   Dim-2 (total free): whole handshake transient (~20 KB) must fit; a board can
 *     clear dim-1 and still OOM (esp32-s2-mini: ~25 KB total).
 */
static bb_err_t ota_heap_guard(const char *stage)
{
    size_t largest    = bb_board_heap_internal_largest_free_block();
    size_t total_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const char *dim   = NULL;
    if (!bb_tls_heap_guard_passes(largest, BB_OTA_HEAP_FLOOR_BYTES,
                                   total_free, BB_TLS_HEAP_TOTAL_FLOOR,
                                   &dim)) {
        bb_log_e(TAG, "OTA refused (%s): %s heap guard failed (largest=%u total_free=%u "
                 "contiguous_floor=%u total_floor=%u)",
                 stage, dim, (unsigned)largest, (unsigned)total_free,
                 (unsigned)BB_OTA_HEAP_FLOOR_BYTES,
                 (unsigned)BB_TLS_HEAP_TOTAL_FLOOR);
        ota_set_error("insufficient heap for OTA (%s/%s guard): largest=%u total_free=%u",
                      stage, dim, (unsigned)largest, (unsigned)total_free);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#endif

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

    bb_wdt_extend_begin(CONFIG_BB_OTA_PULL_WDT_EXTENDED_S);

#if BB_OTA_HEAP_FLOOR_BYTES > 0 || BB_TLS_HEAP_TOTAL_FLOOR > 0
    // Pre-flight heap guard (before wifi check + PM-lock). Best-effort — see
    // ota_heap_guard(); a second guard runs pre-handshake below.
    {
        bb_err_t g = ota_heap_guard("pre-flight");
        if (g != ESP_OK) { ret = g; goto done; }
    }
#endif

    // WiFi IP pre-flight — poll until the IP stack settles or we time out.
    // Catches OTA attempts fired during reconnect or seconds after reboot,
    // before DHCP has bound an address (esp32-c3 single-core path can take a
    // few seconds). Poll in 500 ms steps up to CONFIG_BB_OTA_PULL_WIFI_SETTLE_MS;
    // on a healthy board the first poll passes and there is no added delay.
    {
        const uint32_t step_ms  = 500;
        const uint32_t limit_ms = CONFIG_BB_OTA_PULL_WIFI_SETTLE_MS;
        uint32_t waited_ms = 0;
        while (!bb_wifi_has_ip()) {
            if (waited_ms >= limit_ms) {
                bb_log_e(TAG, "wifi not ready after %u ms — aborting OTA", (unsigned)waited_ms);
                ota_set_error("wifi not ready");
                goto done;
            }
            bb_log_w(TAG, "wifi has no IP — waiting (elapsed %u/%u ms)",
                     (unsigned)waited_ms, (unsigned)limit_ms);
            vTaskDelay(pdMS_TO_TICKS(step_ms));
            waited_ms += step_ms;
        }
        if (waited_ms > 0) {
            bb_log_i(TAG, "wifi settled after %u ms", (unsigned)waited_ms);
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
    bb_ota_emit_progress("pull", BB_OTA_PHASE_START, 0);

    // Pre-resolve any cross-host redirect (e.g. GitHub → CDN) before handing
    // the URL to esp_https_ota.  The chunked Range-request path does not
    // reliably follow 302s across hosts; resolving here gives esp_https_ota the
    // final CDN URL directly.  Falls back to asset_url on any probe failure.
    // (B1-354: regression introduced when partial_http_download was enabled)
    char s_redirect_buf[512];
    const char *download_url = ota_resolve_redirect(asset_url,
                                                    s_redirect_buf,
                                                    sizeof(s_redirect_buf));

    esp_http_client_config_t http_config = {
        .url = download_url,
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
         * peak allocation from ~17 KB to ~9 KB.
         *
         * Chunk size is CONFIG_BB_OTA_PULL_HTTP_CHUNK_SIZE (default 4096). With
         * DYNAMIC_BUFFER the inbound record buffer is re-allocated per TLS record
         * mid-transfer; a 4 KB chunk needs a ~4.4 KB contiguous block each record,
         * which fails once the heap fragments below that under concurrent
         * httpd/SSE load on no-PSRAM boards. Those boards set the chunk to 2048
         * (~2.2 KB per-record alloc) for a far more fragmented-heap-tolerant pull;
         * PSRAM boards keep 4096 for fewer round-trips. */
        .partial_http_download = true,
        .max_http_request_size = CONFIG_BB_OTA_PULL_HTTP_CHUNK_SIZE,
    };

    // Verify OTA partition exists before attempting
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        bb_log_e(TAG, "no OTA update partition found");
        ota_set_error("no OTA update partition (check partition table)");
        goto done_release;
    }
    bb_log_i(TAG, "OTA target partition: %s", update_partition->label);

#if BB_OTA_HEAP_FLOOR_BYTES > 0 || BB_TLS_HEAP_TOTAL_FLOOR > 0
    // Second heap guard — PM lock held and wifi confirmed — re-checked right
    // before the TLS handshake to catch fragmentation since pre-flight.
    {
        bb_err_t g = ota_heap_guard("pre-handshake");
        if (g != ESP_OK) { ret = g; goto done_release; }
    }
#endif

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
    const int max_dl_attempts = CONFIG_BB_OTA_PULL_DL_MAX_ATTEMPTS;
    bool download_ok = false;

    for (int dl = 0; dl < max_dl_attempts && !download_ok; dl++) {
        if (dl > 0) {
            bb_log_i(TAG, "OTA download attempt %d/%d (backoff %u ms)",
                     dl + 1, max_dl_attempts, (unsigned)ota_dl_backoff_ms(dl - 1));
            vTaskDelay(pdMS_TO_TICKS(ota_dl_backoff_ms(dl - 1)));
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
                vTaskDelay(pdMS_TO_TICKS(ota_dl_backoff_ms(attempt)));
            }
        }
        if (err != ESP_OK) {
            // Handshake exhausted inner retries — count this as one download
            // attempt and let the outer loop decide whether to retry.
            bb_log_w(TAG, "OTA download attempt %d/%d: handshake failed: %s",
                     dl + 1, max_dl_attempts, esp_err_to_name(err));
            // B1-358: emit TLS record-size diagnostic and capture into last_error.
            // esp_https_ota_begin does not expose the underlying mbedtls error
            // after failure (the tls handle is internal to the ota handle which is
            // NULL on failure). Pass BB_TLS_RECORD_TOO_BIG as the assumed
            // code on any begin failure — the actionable SSL_IN hint fires for
            // every begin failure, which is the right default: the dominant
            // field-observed cause on no-PSRAM boards IS the record-size issue,
            // and the message is correct for that class.
            {
                char host_buf[64];
                const char *diag_host = ota_url_host(download_url, host_buf,
                                                     sizeof(host_buf));
                char diag_buf[192];
                bb_tls_handshake_diag(BB_TLS_RECORD_TOO_BIG, diag_host,
                                      BB_TLS_SSL_IN_LEN, diag_buf, sizeof(diag_buf));
                bb_log_w(TAG, "%s", diag_buf);
                ota_set_error("%s", diag_buf);
            }
            // ota_handle is already NULL here (aborted in inner loop)
            continue;
        }

        // --- Board-name mismatch check (deterministic — do NOT retry) ---
        const esp_app_desc_t *running = esp_app_get_description();
        if (strncmp(img_desc.project_name, running->project_name,
                    sizeof(img_desc.project_name)) != 0) {
            bool skip_check = bb_ota_skip_check();
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
                    bb_ota_emit_progress("pull", BB_OTA_PHASE_PROGRESS, pct);
                    last_logged_pct = pct;
                }
            }
            // Warn if no bytes moved in the last 10s — surfaces a stalled
            // socket that would otherwise look like silence in the logs.
            int64_t now_us = esp_timer_get_time();
            if (read_so_far != last_read) {
                last_read = read_so_far;
                last_progress_us = now_us;
            } else if (now_us - last_progress_us > (int64_t)CONFIG_BB_OTA_PULL_STALL_WARN_S * 1000000LL) {
                bb_log_w(TAG, "OTA stalled: no bytes for >%ds at %d bytes",
                         CONFIG_BB_OTA_PULL_STALL_WARN_S, read_so_far);
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
    bb_wdt_extend_end();
    bb_ota_emit_progress("pull", ret == BB_OK ? BB_OTA_PHASE_SUCCESS : BB_OTA_PHASE_FAIL,
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
    bb_err_t wdt_err = bb_wdt_task_unsubscribe();
    if (wdt_err != BB_OK) {
        bb_log_w(TAG, "bb_wdt_task_unsubscribe: %d", wdt_err);
    }

    ota_worker_arg_t result;
    if (arg) {
        memcpy(&result, arg, sizeof(ota_worker_arg_t));
        bb_mem_free(arg);
    } else {
        ota_task_exit();
        return;
    }

    // Cooperatively pause work before the download. The pause window plus the
    // flash phase can exceed CONFIG_ESP_TASK_WDT_TIMEOUT_S; the core extends the
    // WDT (and ota_task_exit restores it).
    bool paused = false;
    bool pause_ok;
    if (bb_ota_has_pause_hook()) { paused = bb_ota_pause(); pause_ok = paused; } else { pause_ok = true; }
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
    if (paused) { bb_ota_resume(); }
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
    bb_wdt_task_unsubscribe();
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
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

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

    // Cache-first: trust the cached update-check result when fresh; otherwise
    // kick the worker and wait for a fresh check before deciding.
    // NOTE: the OTA claim is NOT held here — bb_update_check_run_blocking()
    // spawns an upd_check worker that acquires the same claim ("upd_check").
    // Holding the claim before the refresh would deadlock: upd_check skips,
    // refresh times out, apply returns "check_failed" and leaks the claim.
    bb_update_check_status_t uc_status;
    bb_err_t uc_err = bb_update_check_get_status(&uc_status);
    if (uc_err != BB_OK) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "update_check_unavailable");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    int64_t now_us = esp_timer_get_time();
    bool fresh = bb_ota_pull_apply_cache_is_fresh(uc_status.last_check_ok,
                                                  uc_status.last_check_us,
                                                  now_us,
                                                  CONFIG_BB_OTA_PULL_APPLY_CACHE_FRESH_S);
    if (!fresh) {
        bb_log_i(TAG, "apply: cache stale (last_check_us=%" PRId64 "); refreshing",
                 uc_status.last_check_us);
        bb_err_t block_err = bb_update_check_run_blocking(BB_OTA_APPLY_REFRESH_TIMEOUT_MS);
        if (block_err != BB_OK) {
            taskENTER_CRITICAL(&s_ota_status_mux);
            s_ota_in_progress = false;
            taskEXIT_CRITICAL(&s_ota_status_mux);
            bb_log_w(TAG, "apply: refresh failed: %d", block_err);
            bb_http_resp_set_status(req, 503);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "check_failed");
            bb_http_resp_json_obj_end(&obj);
            return BB_OK;
        }
        // Re-read the now-fresh status.
        uc_err = bb_update_check_get_status(&uc_status);
        if (uc_err != BB_OK || !uc_status.last_check_ok) {
            taskENTER_CRITICAL(&s_ota_status_mux);
            s_ota_in_progress = false;
            taskEXIT_CRITICAL(&s_ota_status_mux);
            bb_http_resp_set_status(req, 503);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "check_failed");
            bb_http_resp_json_obj_end(&obj);
            return BB_OK;
        }
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
#if CONFIG_BB_OTA_STATIC_STACK && CONFIG_BB_OTA_PULL_AUTOREGISTER
    ota_worker_arg_t *task_arg = &s_ota_worker_arg;
#else
    ota_worker_arg_t *task_arg = bb_malloc_prefer_spiram(sizeof(ota_worker_arg_t));
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
#endif

    // Claim the OTA exclusive slot immediately before spawning the worker.
    // All early-return paths above (refresh, no-update-available, alloc) occur
    // BEFORE this acquire, so they cannot leak the claim.
    if (bb_update_check_ota_claim_acquire("ota_pull") != BB_OK) {
        taskENTER_CRITICAL(&s_ota_status_mux);
        s_ota_in_progress = false;
        taskEXIT_CRITICAL(&s_ota_status_mux);
#if !(CONFIG_BB_OTA_STATIC_STACK && CONFIG_BB_OTA_PULL_AUTOREGISTER)
        bb_mem_free(task_arg);
#endif
        bb_log_w(TAG, "apply: ota_pull claim conflict (upd_check in flight)");
        bb_http_resp_set_status(req, 409);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "ota_op_in_progress");
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
#if CONFIG_BB_OTA_STATIC_STACK && CONFIG_BB_OTA_PULL_AUTOREGISTER
    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        ota_worker_task,
        "ota_pull",
        OTA_TASK_STACK / sizeof(StackType_t),
        task_arg,
        s_ota_task_prio,
        s_ota_pull_stack,
        &s_ota_pull_task_buf,
        ota_task_core
    );
    if (!task_handle) {
        bb_update_check_ota_claim_release("ota_pull");
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
#else
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
        bb_update_check_ota_claim_release("ota_pull");
        bb_mem_free(task_arg);
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
#endif

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
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

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
    .handler  = ota_check_handler,
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
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "allocation failure or OTA worker task create failed" },
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
    .handler  = ota_update_handler,
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
    .handler  = ota_status_handler,
};

/**
 * Register OTA pull HTTP handlers with an existing httpd instance.
 */
static bb_err_t bb_ota_pull_init(bb_http_handle_t server)
{
    if (!server) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t err = bb_http_register_described_route(server, &s_ota_check_route);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/update/check handler");
        return err;
    }

    err = bb_http_register_described_route(server, &s_ota_update_route);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/update/apply handler");
        return err;
    }

    err = bb_http_register_described_route(server, &s_ota_status_route);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/update/progress handler");
        return err;
    }

    bb_log_i(TAG, "OTA pull handlers registered");
    return BB_OK;
}

#if CONFIG_BB_OTA_PULL_AUTOREGISTER
static bb_err_t bb_ota_pull_reserve_routes(void)
{
    bb_http_reserve_routes(3);  // POST /api/update/check + POST /api/update/apply + GET /api/update/progress
    return BB_OK;
}
BB_INIT_REGISTER_PRE_HTTP(bb_ota_pull, bb_ota_pull_reserve_routes);
BB_INIT_REGISTER_N(bb_ota_pull, bb_ota_pull_init, 3);
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
