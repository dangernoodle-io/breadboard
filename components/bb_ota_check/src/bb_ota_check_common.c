#include "bb_ota_check.h"
#include "bb_ota_check_internal.h"
#include "bb_release_manifest.h"
#include "bb_cache.h"
#include "bb_clock.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_client.h"
#include "bb_data.h"
#include "bb_log.h"
#include "bb_mdns.h"
#include "bb_settings.h"
#include "bb_openapi.h"
#include "bb_serialize.h"
#include "bb_system.h"
#include "bb_mem.h"
#include "bb_str.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "bb_ota_check";

static const char k_update_available_schema[] =
    "{\"title\":\"UpdateAvailable\",\"x-sse-topic\":\"update.available\","
    "\"type\":\"object\","
    "\"properties\":{"
    "\"current\":{\"type\":\"string\"},"
    "\"latest\":{\"type\":\"string\"},"
    "\"download_url\":{\"type\":\"string\"},"
    "\"available\":{\"type\":\"boolean\"},"
    "\"ts\":{\"type\":\"integer\"},"
    "\"last_check_ok\":{\"type\":\"boolean\"},"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"outcome\":{\"type\":\"string\"},"
    "\"last_check_ts\":{\"type\":\"integer\"}},"
    "\"required\":[\"current\",\"latest\",\"download_url\",\"available\","
    "\"ts\",\"last_check_ok\",\"enabled\",\"outcome\"]}";


#ifndef CONFIG_BB_OTA_CHECK_INTERVAL_S
#define CONFIG_BB_OTA_CHECK_INTERVAL_S 21600
#endif

#define URL_MAX        256
#define BOARD_MAX       64
#define BOARD_NAME_FALLBACK "unknown"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool                          s_initialized = false;
static bb_ota_check_cfg_t s_cfg;
static char                          s_url[URL_MAX];
static char                          s_firmware_board[BOARD_MAX];
static bb_release_manifest_parse_fn  s_parser = bb_release_manifest_parse_github;
static bb_ota_check_status_t s_status;
static bool                          s_first_check_done = false;
static pthread_mutex_t               s_lock = PTHREAD_MUTEX_INITIALIZER;
static bb_ota_check_pause_cb_t s_pause_hook = NULL;
static bb_ota_check_resume_cb_t s_resume_hook = NULL;
static int64_t                       s_last_publish_ts = 0;  // wall-clock seconds of last publish_state call

#ifndef ESP_PLATFORM
// On host/Arduino there is no real FreeRTOS task; the in-flight flag is a
// plain atomic so the host kick() stub can respect it for testing.
static atomic_bool                   s_in_flight = false;
#endif

// ---------------------------------------------------------------------------
// Semver compare
// ---------------------------------------------------------------------------

// Parses "vX.Y.Z" or "X.Y.Z". Returns 0 on success, -1 on parse failure.
static int parse_semver(const char *s, int *maj, int *min, int *patch)
{
    if (!s || !*s) return -1;  // LCOV_EXCL_BR_LINE — defensive against empty/NULL
    if (*s == 'v' || *s == 'V') s++;  // LCOV_EXCL_BR_LINE — 'V' upper-case uncommon
    int a = 0, b = 0, c = 0;
    int matched = sscanf(s, "%d.%d.%d", &a, &b, &c);
    if (matched < 3) return -1;  // LCOV_EXCL_BR_LINE — malformed tag defensive
    if (a < 0 || b < 0 || c < 0) return -1;  // LCOV_EXCL_BR_LINE — negative components defensive
    *maj = a; *min = b; *patch = c;
    return 0;
}

// >0 if a > b, 0 if equal, <0 if a < b, INT_MIN on parse failure.
// "dev"-prefixed running versions are always treated as older.
static int semver_compare(const char *a, const char *b)
{
    if (!a || !b) return INT_MIN;  // LCOV_EXCL_BR_LINE — defensive
    bool a_dev = (strncmp(a, "dev", 3) == 0);
    bool b_dev = (strncmp(b, "dev", 3) == 0);
    if (a_dev && b_dev) return 0;  // LCOV_EXCL_BR_LINE — both-dev unreachable in tests
    if (a_dev) return -1;
    if (b_dev) return 1;  // LCOV_EXCL_BR_LINE — running="dev" requires custom host build

    int amaj, amin, apatch, bmaj, bmin, bpatch;
    if (parse_semver(a, &amaj, &amin, &apatch) < 0) return INT_MIN;  // LCOV_EXCL_BR_LINE — parser rejects upstream
    if (parse_semver(b, &bmaj, &bmin, &bpatch) < 0) return INT_MIN;  // LCOV_EXCL_BR_LINE — running version always parseable on host
    if (amaj != bmaj) return amaj - bmaj;
    if (amin != bmin) return amin - bmin;  // LCOV_EXCL_BR_LINE — minor differ untested
    return apatch - bpatch;
}

// ---------------------------------------------------------------------------
// Forward declaration — outcome_str is defined later in this file.
// ---------------------------------------------------------------------------
static const char *outcome_str(bb_ota_check_outcome_t o);

// ---------------------------------------------------------------------------
// mDNS publish + bb_cache update + SSE post
// ---------------------------------------------------------------------------

static void publish_state(const bb_ota_check_status_t *st, const char *txt_value)
{
    bb_mdns_set_txt("update", txt_value);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    s_last_publish_ts = (int64_t)tv.tv_sec;

    bb_ota_check_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    bb_strlcpy(snap.current,      st->current,      sizeof(snap.current));
    bb_strlcpy(snap.latest,       st->latest,       sizeof(snap.latest));
    bb_strlcpy(snap.download_url, st->download_url, sizeof(snap.download_url));
    snap.available     = st->available;
    snap.ts            = s_last_publish_ts;
    snap.last_check_ok = st->last_check_ok;
    snap.enabled       = bb_settings_update_check_enabled_get();
    bb_strlcpy(snap.outcome, outcome_str(st->outcome), sizeof(snap.outcome));
    snap.last_check_ts = (st->last_check_us != 0) ? (int64_t)(st->last_check_us / 1000000) : 0;

    bb_cache_update(&(bb_cache_update_t){ .key = BB_OTA_CHECK_TOPIC, .snap = &snap });
    bb_data_touch(BB_OTA_CHECK_TOPIC);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_ota_check_init(const bb_ota_check_cfg_t *cfg)
{
    if (s_initialized) return BB_OK;

    s_cfg.interval_s   = (cfg && cfg->interval_s) ? cfg->interval_s : CONFIG_BB_OTA_CHECK_INTERVAL_S;  // LCOV_EXCL_BR_LINE — cfg permutations covered above
    s_cfg.post_initial = (cfg && cfg->post_initial);  // LCOV_EXCL_BR_LINE

    memset(&s_status, 0, sizeof(s_status));
    const char *running = bb_system_get_version();
    if (running) {  // LCOV_EXCL_BR_LINE — host always returns non-null
        bb_strlcpy(s_status.current, running, sizeof(s_status.current));
    }
    s_status.latest[0] = '\0';
    s_status.download_url[0] = '\0';
    s_status.last_check_us = 0;
    s_status.last_check_ok = false;
    s_status.available = false;
    s_status.outcome = BB_OTA_CHECK_OUTCOME_UNKNOWN;
    s_first_check_done = false;
    s_url[0] = '\0';
    s_firmware_board[0] = '\0';
    s_parser = bb_release_manifest_parse_github;

    // SSE/broadcast delivery is a bb_data/bb_data_http composition-root
    // concern (B1-1045); the REST GET path is now bb_data too (B1-1053 PR3)
    // -- bb_cache here is purely the snapshot store bb_ota_check_gather()
    // reads via bb_cache_get_raw(), no .serialize slot, BB_CACHE_FLAG_NONE.
    bb_cache_config_t cache_cfg = {
        .key       = BB_OTA_CHECK_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_ota_check_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cache_cfg);
    // LCOV_EXCL_START — cache_register failure is defensive (NO_SPACE only)
    if (err != BB_OK) {
        return err;
    }
    // LCOV_EXCL_STOP

    // Bind "update.available" to bb_data so bb_ota_check_emit_status_json's
    // bb_data_render() call can resolve it. Called here (not at either
    // route-registration call site) because bb_ota_check_init() is the one
    // function both bb_ota_check_register_init() (persistent composition
    // path) and bb_ota_boot's status_check_ensure_init() (boot-mode path,
    // platform/espidf/bb_ota_boot/bb_ota_boot.c) already call -- binding here
    // covers GET /api/update/status on both without a second call site.
    // Non-fatal like bb_diag_boot_bind()'s call site: a bind failure (e.g.
    // BB_DATA_MAX_BINDINGS already full) means REST reads BB_ERR_NOT_FOUND
    // until re-bound, but does not block bb_ota_check itself from working.
    {
        bb_err_t derr = bb_ota_check_bind();
        if (derr != BB_OK) {
            bb_log_w(TAG, "bb_ota_check_bind failed: %d", (int)derr);
        }
    }

    bb_mdns_set_txt("update", "unknown");

    bb_openapi_register_topic_schema(BB_OTA_CHECK_TOPIC, k_update_available_schema,
                                     "UpdateAvailable");

    s_initialized = true;

    bb_log_i(TAG, "initialized: interval=%us", (unsigned)s_cfg.interval_s);
    return BB_OK;
}

bb_err_t bb_ota_check_publish_initial(void)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    bb_ota_check_status_t snap = s_status;
    pthread_mutex_unlock(&s_lock);
    publish_state(&snap, "unknown");
    return BB_OK;
}

bb_err_t bb_ota_check_set_releases_url(const char *url)
{
    if (!url) return BB_ERR_INVALID_ARG;
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    size_t n = strlen(url);
    if (n == 0 || n >= URL_MAX) return BB_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    bb_strlcpy(s_url, url, sizeof(s_url));
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_ota_check_set_parser(bb_release_manifest_parse_fn fn)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    s_parser = fn ? fn : bb_release_manifest_parse_github;
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_ota_check_set_firmware_board(const char *board)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    // NULL/empty clears to default; non-empty must fit the internal buffer.
    if (board && board[0] != '\0' && strlen(board) >= BOARD_MAX) return BB_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    if (!board || board[0] == '\0') {
        s_firmware_board[0] = '\0';
    } else {
        bb_strlcpy(s_firmware_board, board, sizeof(s_firmware_board));
    }
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_ota_check_set_hooks(bb_ota_check_pause_cb_t pause,
                                   bb_ota_check_resume_cb_t resume)
{
    // Order-independent: boot-strategy boards lazily init bb_ota_check
    // (via bb_ota_boot) AFTER the consumer wires hooks in app_main. Store the
    // hooks regardless of init so they survive the later init (which does not
    // clear them) and are present when run_one fires. Pre-init there is no
    // worker task yet, so the store is single-threaded — skip the lock; once
    // initialized, take the lock as before.
    if (!s_initialized) {
        s_pause_hook  = pause;
        s_resume_hook = resume;
        return BB_OK;
    }
    pthread_mutex_lock(&s_lock);
    s_pause_hook  = pause;
    s_resume_hook = resume;
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_ota_check_get_status(bb_ota_check_status_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    *out = s_status;
    const char *board_eff = (s_firmware_board[0] != '\0') ? s_firmware_board : BOARD_NAME_FALLBACK;
    bb_strlcpy(out->board, board_eff, sizeof(out->board));
    pthread_mutex_unlock(&s_lock);
    out->enabled = bb_settings_update_check_enabled_get();
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Streaming chunk callback context for the default GitHub parser
// ---------------------------------------------------------------------------

typedef struct {
    bb_release_manifest_stream_ctx_t stream_ctx;
    bb_err_t                         parse_err;
} stream_feed_ctx_t;

static bb_err_t chunk_cb(void *cv, const char *data, size_t len)
{
    stream_feed_ctx_t *fc = (stream_feed_ctx_t *)cv;
    bb_err_t err = bb_release_manifest_parse_github_stream_feed(&fc->stream_ctx, data, len);
    if (err != BB_OK) {  // LCOV_EXCL_BR_LINE — feed only errors on BB_ERR_NO_SPACE (oversized buffers)
        fc->parse_err = err;  // LCOV_EXCL_LINE — feed only errors on BB_ERR_NO_SPACE (buffers oversized)
        return err;           // LCOV_EXCL_LINE
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Streaming chunk callback context for custom (buffered) parsers
// ---------------------------------------------------------------------------

#ifdef CONFIG_BB_OTA_CHECK_CUSTOM_PARSER_BUF_BYTES
#define CUSTOM_PARSER_BUF_SIZE CONFIG_BB_OTA_CHECK_CUSTOM_PARSER_BUF_BYTES
#else
#define CUSTOM_PARSER_BUF_SIZE 8192
#endif

typedef struct {
    char    *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;
} buf_ctx_t;

static bb_err_t buf_chunk_cb(void *cv, const char *data, size_t n)
{
    buf_ctx_t *bc = (buf_ctx_t *)cv;
    if (bc->overflow) return BB_OK;
    size_t avail = bc->cap - bc->len;
    size_t copy  = n < avail ? n : avail;
    if (copy > 0) {
        memcpy(bc->buf + bc->len, data, copy);
        bc->len += copy;
    }
    if (n > avail) {
        bc->overflow = true;
        bb_log_w(TAG, "custom parser buffer overflow: response truncated at %zu bytes; raise CONFIG_BB_OTA_CHECK_CUSTOM_PARSER_BUF_BYTES", bc->cap);
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Synchronous one-shot check
// ---------------------------------------------------------------------------

bb_err_t bb_ota_check_run_one(void)
{
    if (!s_initialized) return BB_ERR_INVALID_ARG;

    if (!bb_settings_update_check_enabled_get()) {
        bb_log_i(TAG, "update check disabled; skipping");
        return BB_OK;
    }

    char url_local[URL_MAX];
    char board_local[BOARD_MAX];
    bb_release_manifest_parse_fn parser_local;
    bb_ota_check_pause_cb_t  pause_local;
    bb_ota_check_resume_cb_t resume_local;
    pthread_mutex_lock(&s_lock);
    bb_strlcpy(url_local, s_url, sizeof(url_local));
    bb_strlcpy(board_local, s_firmware_board, sizeof(board_local));
    parser_local  = s_parser;
    pause_local   = s_pause_hook;
    resume_local  = s_resume_hook;
    pthread_mutex_unlock(&s_lock);

    const char *board_name = (board_local[0] != '\0') ? board_local : BOARD_NAME_FALLBACK;

    if (url_local[0] == '\0') return BB_ERR_INVALID_STATE;

    char tag[24]    = {0};
    char dl_url[256] = {0};
    bb_http_client_result_t res = {0};
    bb_err_t err;
    bb_err_t perr;

    if (parser_local == bb_release_manifest_parse_github) {
        // Default path: fully streaming, no body buffer.
        stream_feed_ctx_t fc;
        memset(&fc, 0, sizeof(fc));
        fc.parse_err = BB_OK;

        perr = bb_release_manifest_parse_github_stream_begin(
            &fc.stream_ctx, board_name,
            tag, sizeof(tag), dl_url, sizeof(dl_url));
        if (perr != BB_OK) {  // LCOV_EXCL_BR_LINE — args always valid here
            return perr;      // LCOV_EXCL_LINE
        }

        if (pause_local && !pause_local()) {
            bb_log_w(TAG, "pause hook refused; skipping fetch");
            return BB_ERR_INVALID_STATE;
        }
        err = bb_http_client_get_stream(url_local, chunk_cb, &fc, NULL, &res);
        if (resume_local) resume_local();

        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t now_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;

        if (err != BB_OK || res.status_code < 200 || res.status_code >= 300) {  // LCOV_EXCL_BR_LINE — short-circuits across err/status branches
            pthread_mutex_lock(&s_lock);
            s_status.last_check_us = now_us;
            s_status.last_check_ok = false;
            s_status.outcome = BB_OTA_CHECK_OUTCOME_FAILED;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "fetch failed: err=%d status=%d", err, res.status_code);
            return err == BB_OK ? BB_ERR_INVALID_STATE : err;
        }

        perr = bb_release_manifest_parse_github_stream_end(&fc.stream_ctx);
        if (perr != BB_OK) {
            pthread_mutex_lock(&s_lock);
            s_status.last_check_us = now_us;
            s_status.last_check_ok = false;
            s_status.outcome = BB_OTA_CHECK_OUTCOME_FAILED;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "parse failed: %d", perr);
            return perr;
        }

        // No-asset terminal: tag parsed successfully but no matching board asset.
        // This is a success outcome (last_check_ok=true) — distinguishable from
        // up_to_date. Consumers polling last_check_ok will not hang.
        if (dl_url[0] == '\0') {
            bb_ota_check_status_t snap;
            pthread_mutex_lock(&s_lock);
            s_status.latest[0] = '\0';
            s_status.download_url[0] = '\0';
            s_status.available = false;
            s_status.last_check_us = now_us;
            s_status.last_check_ok = true;
            s_status.outcome = BB_OTA_CHECK_OUTCOME_NO_ASSET;
            s_first_check_done = true;
            snap = s_status;
            pthread_mutex_unlock(&s_lock);
            bb_log_i(TAG, "update check: no firmware asset for this board");
            publish_state(&snap, "none");
            return BB_OK;
        }

        // Fall through to the version-compare + publish block below.
        // (Extracted tag/dl_url are already filled in by the stream parser.)
        int cmp = semver_compare(tag, s_status.current);
        bool new_available = (cmp != INT_MIN) && (cmp > 0);  // LCOV_EXCL_BR_LINE — INT_MIN path defensive

        bb_ota_check_status_t snap;
        pthread_mutex_lock(&s_lock);
        bb_strlcpy(s_status.latest, tag, sizeof(s_status.latest));
        bb_strlcpy(s_status.download_url, dl_url, sizeof(s_status.download_url));
        s_status.available = new_available;
        s_status.last_check_us = now_us;
        s_status.last_check_ok = true;
        s_status.outcome = new_available ? BB_OTA_CHECK_OUTCOME_AVAILABLE : BB_OTA_CHECK_OUTCOME_UP_TO_DATE;
        s_first_check_done = true;
        snap = s_status;
        pthread_mutex_unlock(&s_lock);

        publish_state(&snap, new_available ? snap.latest : "none");
        return BB_OK;

    } else {
        // Custom parser path: buffer into a local heap allocation.
        buf_ctx_t bc;
        bc.buf = (char *)bb_calloc_prefer_spiram(1, CUSTOM_PARSER_BUF_SIZE);
        if (!bc.buf) {  // LCOV_EXCL_START — OOM defensive
            bb_log_w(TAG, "custom parser buf alloc failed");
            return BB_ERR_NO_SPACE;
        }  // LCOV_EXCL_STOP
        bc.cap      = CUSTOM_PARSER_BUF_SIZE;
        bc.len      = 0;
        bc.overflow = false;

        if (pause_local && !pause_local()) {
            bb_log_w(TAG, "pause hook refused; skipping fetch");
            bb_mem_free(bc.buf);
            return BB_ERR_INVALID_STATE;
        }
        err = bb_http_client_get_stream(url_local, buf_chunk_cb, &bc, NULL, &res);
        if (resume_local) resume_local();

        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t now_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;

        if (err != BB_OK || res.status_code < 200 || res.status_code >= 300) {  // LCOV_EXCL_BR_LINE
            pthread_mutex_lock(&s_lock);
            s_status.last_check_us = now_us;
            s_status.last_check_ok = false;
            s_status.outcome = BB_OTA_CHECK_OUTCOME_FAILED;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "fetch failed: err=%d status=%d", err, res.status_code);
            bb_mem_free(bc.buf);
            return err == BB_OK ? BB_ERR_INVALID_STATE : err;
        }

        perr = parser_local(bc.buf, bc.len, board_name,
                            tag, sizeof(tag), dl_url, sizeof(dl_url));
        bb_mem_free(bc.buf);
        if (perr != BB_OK) {
            pthread_mutex_lock(&s_lock);
            s_status.last_check_us = now_us;
            s_status.last_check_ok = false;
            s_status.outcome = BB_OTA_CHECK_OUTCOME_FAILED;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "parse failed: %d", perr);
            return perr;
        }

        // No-asset terminal: tag parsed successfully but no matching board asset.
        // This is a success outcome (last_check_ok=true) — distinguishable from
        // up_to_date. Consumers polling last_check_ok will not hang.
        if (dl_url[0] == '\0') {
            bb_ota_check_status_t snap;
            pthread_mutex_lock(&s_lock);
            s_status.latest[0] = '\0';
            s_status.download_url[0] = '\0';
            s_status.available = false;
            s_status.last_check_us = now_us;
            s_status.last_check_ok = true;
            s_status.outcome = BB_OTA_CHECK_OUTCOME_NO_ASSET;
            s_first_check_done = true;
            snap = s_status;
            pthread_mutex_unlock(&s_lock);
            bb_log_i(TAG, "update check: no firmware asset for this board");
            publish_state(&snap, "none");
            return BB_OK;
        }

        int cmp = semver_compare(tag, s_status.current);
        bool new_available = (cmp != INT_MIN) && (cmp > 0);  // LCOV_EXCL_BR_LINE

        bb_ota_check_status_t snap;
        pthread_mutex_lock(&s_lock);
        bb_strlcpy(s_status.latest, tag, sizeof(s_status.latest));
        bb_strlcpy(s_status.download_url, dl_url, sizeof(s_status.download_url));
        s_status.available = new_available;
        s_status.last_check_us = now_us;
        s_status.last_check_ok = true;
        s_status.outcome = new_available ? BB_OTA_CHECK_OUTCOME_AVAILABLE : BB_OTA_CHECK_OUTCOME_UP_TO_DATE;
        s_first_check_done = true;
        snap = s_status;
        pthread_mutex_unlock(&s_lock);

        publish_state(&snap, new_available ? snap.latest : "none");
        return BB_OK;
    }
}

bool bb_ota_check_is_initialized(void)
{
    return s_initialized;
}

bb_err_t bb_ota_check_now(void)
{
    if (!s_initialized) return BB_ERR_INVALID_ARG;
    if (s_url[0] == '\0') return BB_ERR_INVALID_STATE;
    return bb_ota_check_run_one();
}

#ifndef ESP_PLATFORM
// Host/Arduino stub: no real FreeRTOS task; kick() is synchronous but
// respects the in-flight guard so tests can verify the no-duplicate-spawn path.
bb_err_t bb_ota_check_kick(void)
{
    // Atomically try to claim the in-flight slot.  If already set, a check is
    // "in flight" — skip (mirrors the ESP-IDF on-demand guard behaviour).
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_in_flight, &expected, true)) {
        bb_log_d(TAG, "kick: check already in flight, skipping");
        return BB_OK;
    }
    bb_err_t err = bb_ota_check_now();
    atomic_store(&s_in_flight, false);
    return err;
}

// Host/Arduino stub: no worker task, so run_blocking is synchronous like now().
bb_err_t bb_ota_check_run_blocking(uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    return bb_ota_check_now();
}

// Host/Arduino stub: no worker task to pin, no-op.
void bb_ota_check_set_task_core(int core)
{
    (void)core;
}

// Host/Arduino stub: no worker task to prioritize, no-op.
void bb_ota_check_set_task_priority(int priority)
{
    (void)priority;
}
#endif

// ---------------------------------------------------------------------------
// GET /api/update/config  — read the runtime opt-out flag
// POST /api/update/config — write the runtime opt-out flag
// Defined here (not in bb_ota_check_espidf.c) so host tests can reach them.
// Route registration is performed by the platform port.
// ---------------------------------------------------------------------------

#define BB_OTA_CHECK_CONFIG_BODY_MAX 64

// ---------------------------------------------------------------------------
// B1-859: POST /api/update/config ingress migrated off bb_json onto
// bb_data_apply -- the sibling of the B1-859 factory-reset cutover
// (bb_storage_http_routes.c). This site does NOT fit that template
// unmodified: the old handler's single field is a JSON *bool*
// ("enabled"), and bb_serialize_populate() leaves an ABSENT field's
// destination bytes untouched rather than clearing them -- there is no
// "empty string" fallback the way factory-reset's confirm field had, so a
// plain BB_DATA_APPLY_POST (zero-seed) cannot distinguish an omitted
// "enabled" from an explicit `{"enabled":false}` (both would scatter to
// the same zeroed byte).
//
// Sentinel fix (user-decided, B1-859): apply via BB_DATA_APPLY_PATCH
// instead, whose seed step actually calls this binding's gather() hook
// (unlike the wifi/factory-reset POST-mode cutovers, where gather() only
// exists to satisfy bb_data_bind()'s non-NULL invariant and is otherwise
// dead). gather() seeds the destination byte with
// BB_OTA_CHECK_CONFIG_ENABLED_UNSET, a value BB_TYPE_BOOL's populate path
// can never itself produce (the JSON backend's get_bool getter always
// hands populate a genuine C `bool`, i.e. exactly 0 or 1, before the
// memcpy into this byte -- see bb_serialize_populate.c's BB_TYPE_BOOL
// case). If "enabled" is present in the body, populate overwrites the
// sentinel with 0/1; if absent, populate leaves it untouched and
// config_apply() below still sees the sentinel -- reproducing the old
// bb_json_obj_get_bool()-returned-false reject-on-missing behavior.
//
// The destination struct field is deliberately `uint8_t`, not `bool`: a
// real C `_Bool` can only ever hold 0 or 1 (assigning 2 to it silently
// clamps to 1), which would destroy the sentinel. `uint8_t` is
// layout-compatible with `bool` for this purpose (both are 1-byte scalars
// on every host/ESP-IDF target this project builds for), which is exactly
// what BB_TYPE_BOOL's populate step needs to memcpy(sizeof(bool)) into.
#define BB_OTA_CHECK_CONFIG_ENABLED_UNSET ((uint8_t)2)

typedef struct {
    uint8_t enabled;
} bb_ota_check_config_apply_t;

static const bb_serialize_field_t s_config_fields[] = {
    { .key = "enabled", .type = BB_TYPE_BOOL, .offset = offsetof(bb_ota_check_config_apply_t, enabled) },
};

static const bb_serialize_desc_t s_config_desc = {
    .type_name = "bb_ota_check_config_apply_t",
    .fields    = s_config_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bb_ota_check_config_apply_t),
};

// Egress hook / PATCH seed: seeds `dst` with the sentinel above so an
// absent "enabled" field survives bb_serialize_populate() untouched and is
// rejected below -- see the banner comment above. Unlike the wifi/
// factory-reset POST-mode cutovers, this hook is NOT dead: bb_data_apply()
// actually calls it (BB_DATA_APPLY_PATCH mode) on every request.
static bb_err_t config_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    ((bb_ota_check_config_apply_t *)dst)->enabled = BB_OTA_CHECK_CONFIG_ENABLED_UNSET;
    return BB_OK;
}

// Ingress hook: rejects a still-sentinel "enabled" (missing or wrong-typed
// field -- the JSON backend's get_bool getter also returns false, leaving
// the seed untouched, on a type mismatch) with BB_ERR_VALIDATION (400,
// same domain-level-reject contract as the factory-reset/wifi apply
// hooks), otherwise durably persists the flag via bb_settings.
static bb_err_t config_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    const bb_ota_check_config_apply_t *cfg = (const bb_ota_check_config_apply_t *)snap;

    if (cfg->enabled == BB_OTA_CHECK_CONFIG_ENABLED_UNSET) {
        return BB_ERR_VALIDATION;
    }

    return bb_settings_update_check_enabled_set(cfg->enabled != 0);
}

// Binds the "ota_check_config" bb_data key against the production gather/
// apply hooks above. Portable (no bb_http_handle_t server dependency,
// unlike bb_storage_http's factory-reset binding) -- called both by
// bb_ota_check_register_init() (ESP-IDF composition root) and directly by
// host tests after bb_data_test_reset().
bb_err_t bb_ota_check_config_bind(void)
{
    bb_data_binding_t binding = {
        .key    = "ota_check_config",
        .desc   = &s_config_desc,
        .gather = config_gather,
        .apply  = config_apply,
    };
    return bb_data_bind(&binding);
}

bb_err_t bb_ota_check_config_get_handler(bb_http_request_t *req)
{
    bool enabled = bb_settings_update_check_enabled_get();
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;  // LCOV_EXCL_BR_LINE — send_chunk never fails on host
    bb_http_resp_json_obj_set_bool(&obj, "enabled", enabled);
    return bb_http_resp_json_obj_end(&obj);
}

bb_err_t bb_ota_check_config_post_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > BB_OTA_CHECK_CONFIG_BODY_MAX) {  // LCOV_EXCL_BR_LINE — both sub-branches exercised but gcov counts as separate
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);  // LCOV_EXCL_BR_LINE — send_chunk never fails on host
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid request");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    char body[BB_OTA_CHECK_CONFIG_BODY_MAX + 1];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {  // LCOV_EXCL_BR_LINE — recv failure path; both branches covered but gcov misattributes inner begin() arc
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "read failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }
    body[n] = '\0';

    // JSON parse scratch: a flat 1-bool-field document, comfortably under
    // this route's own BB_OTA_CHECK_CONFIG_BODY_MAX (64) body cap -- but the
    // token recorder's own default-capacity pool alone is
    // 48 * sizeof(bb_serialize_json_tok_t) == 2304 bytes, so this must clear
    // that plus the control structs plus headroom for the escape-decode
    // arena regardless of body size (see bb_wifi_http_routes.c's
    // WIFI_PATCH_PARSE_SCRATCH_BYTES / the factory-reset cutover's
    // FACTORY_RESET_PARSE_SCRATCH_BYTES, the same fixed-pool-size rationale).
    bb_ota_check_config_apply_t dst_scratch;
    char                        parse_scratch[3072];
    bb_data_apply_req_t apply_req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "ota_check_config",
        .mode              = BB_DATA_APPLY_PATCH,
        .body              = body,
        .body_len          = (size_t)n,
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = &dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };
    bb_err_t rc = bb_data_apply(&apply_req);

    // Response shaping stays here in the route -- bb_data_apply()/apply()
    // stay HTTP-agnostic and never see a status code, only a bb_err_t (same
    // Fork-3 posture as the wifi PATCH and factory-reset POST cutovers).
    // BB_ERR_VALIDATION covers a missing/wrong-typed "enabled" (config_apply()'s
    // own sentinel check) -- "the request body parsed fine but its content
    // is bad", 400. BB_ERR_PARSE_GRAMMAR/BB_ERR_PARSE_INCOMPLETE cover a body
    // the JSON backend's parse fn couldn't scan to completion
    // (grammar-invalid, e.g. "not-json", or truncated mid-object) -- same
    // "the request body itself is bad" bucket, also 400. These are the
    // disjoint parse-layer codes from bb_core.h (#955's fix). Mapping them
    // here is pre-emptive, not a repair: on the pre-B1-859 bb_json path,
    // this handler had never called bb_data_apply() at all, and both a
    // grammar-invalid and a truncated body already produced a NULL doc and
    // hit the same "!doc -> 400" branch, so neither was ever broken for
    // this endpoint. The real "truncated body aliases BB_ERR_INVALID_STATE
    // and falls through to a generic 500" bug belonged to the wifi PATCH
    // endpoint (#953 cut it onto bb_data_apply() before the disjoint codes
    // existed; #955 fixed it there, factory-reset followed in #956). This
    // mapping simply preserves the old bb_json handler's existing
    // 400-for-any-unparseable-body behavior across the migration. Everything
    // else (including a genuine bb_settings NV-write failure, or the
    // composition-invariant case of this fn's own "ota_check_config"
    // binding somehow being unbound/unparsable -- unreachable given correct
    // wiring in bb_ota_check_config_bind()) is a 500.
    if (rc == BB_ERR_VALIDATION) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "missing or invalid 'enabled'");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }
    if (rc == BB_ERR_PARSE_GRAMMAR || rc == BB_ERR_PARSE_INCOMPLETE) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid JSON");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }
    if (rc != BB_OK) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "update failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;  // LCOV_EXCL_BR_LINE — send_chunk never fails on host
    bb_http_resp_json_obj_set_bool(&obj, "enabled", bb_settings_update_check_enabled_get());
    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_config_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
       "\"properties\":{\"enabled\":{\"type\":\"boolean\"}},"
       "\"required\":[\"enabled\"]}",
      "current update-check opt-out state" },
    { 0 },
};

static const bb_route_t s_config_get_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/update/config",
    .tag      = "update",
    .summary  = "Get update-check enabled flag",
    .responses = s_config_get_responses,
    .handler  = bb_ota_check_config_get_handler,
};

static const bb_route_response_t s_config_post_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
       "\"properties\":{\"enabled\":{\"type\":\"boolean\"}},"
       "\"required\":[\"enabled\"]}",
      "updated state" },
    { 400, "text/plain", NULL, "missing or invalid body" },
    { 500, "text/plain", NULL, "update failed" },
    { 0 },
};

static const bb_route_t s_config_post_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/update/config",
    .tag                  = "update",
    .summary              = "Set update-check enabled flag",
    .request_content_type = "application/json",
    .request_schema       =
        "{\"type\":\"object\","
         "\"properties\":{\"enabled\":{\"type\":\"boolean\"}},"
         "\"required\":[\"enabled\"]}",
    .responses = s_config_post_responses,
    .handler  = bb_ota_check_config_post_handler,
};

const bb_route_t *bb_ota_check_config_get_route(void)  { return &s_config_get_route; }
const bb_route_t *bb_ota_check_config_post_route(void) { return &s_config_post_route; }

// ---------------------------------------------------------------------------
// bb_ota_check_emit_status_json — shared HTTP response emitter
// ---------------------------------------------------------------------------

// Map outcome enum to the JSON string value for /api/update/status.
// These exact strings are part of the public API consumed by TaipanMiner webui
// and taipan-cli. Do not rename without a coordinated consumer update.
static const char *outcome_str(bb_ota_check_outcome_t o)
{
    switch (o) {
        case BB_OTA_CHECK_OUTCOME_UP_TO_DATE:    return "up_to_date";
        case BB_OTA_CHECK_OUTCOME_AVAILABLE:     return "available";
        case BB_OTA_CHECK_OUTCOME_NO_ASSET:      return "no_asset";
        case BB_OTA_CHECK_OUTCOME_FAILED:        return "check_failed";
        case BB_OTA_CHECK_OUTCOME_CHECK_ON_APPLY: return "check_on_apply";
        default:                              return "unknown";
    }
}

bb_err_t bb_ota_check_mark_check_on_apply(void)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    s_status.outcome       = BB_OTA_CHECK_OUTCOME_CHECK_ON_APPLY;
    s_status.available     = false;
    s_status.last_check_ok = false;
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

// Derivation (per-field worst-case JSON bytes, "key":value + trailing
// comma, all 9 bb_ota_check_wire_desc fields present -- last_check_ts is
// rendered whenever its .present hook sees last_check_us != 0):
//   current       -- char[24], bb_strlcpy always NUL-terminates -> 23-char
//                    max content, no escaping assumed (build-version
//                    string, not adversarial)            = 10 + 25 = 35
//   latest        -- char[24], same reasoning             = 9  + 25 = 34
//   download_url  -- char[256] -> 255-char max content, EXTERNALLY SOURCED
//                    (fetched from the GitHub release manifest via
//                    bb_ota_check_set_releases_url(); bb_release_manifest_
//                    json_sink.c dequotes JSON escapes on ingest, so a
//                    malicious/misconfigured feed can genuinely land
//                    '"'/'\\' bytes here) -- worst case every byte escapes
//                    (bb_json_escape_write() doubles each)
//                                                          = 15 + 512 = 527
//   available     -- bool, "false" widest               = 12 + 5  = 17
//   ts            -- int64_t, assigned directly (wall-clock seconds, no
//                    derivation constraint) -> full type width,
//                    INT64_MIN = "-9223372036854775808" (20 chars)
//                                                          = 5  + 20 = 25
//   last_check_ok -- bool, "false" widest                = 16 + 5  = 21
//   enabled       -- bool, "false" widest (today's only caller renders
//                    "true"=4, 1 byte narrower)           = 10 + 5  = 15
//   outcome       -- char[24], but only ever populated via outcome_str()'s
//                    fixed literal set -- longest is "check_on_apply" (14
//                    chars), NOT the 24-byte buffer size  = 10 + 16 = 26
//   last_check_ts -- int64_t; in practice derived as
//                    last_check_us/1000000 (truncating division), which
//                    bounds it tighter than ts's full width, but the field
//                    is declared int64_t with no compile-time bound on how
//                    a future producer might populate it -> sized off the
//                    TYPE, same as ts                     = 16 + 20 = 36
// Structural overhead: 8 commas (9 fields) + 2 braces         = 10
// Total: 35+34+527+17+25+21+15+26+36+10 = 746 bytes -- margin 278 against
// this 1024-byte buffer (1023 usable, 1 byte reserved for the NUL
// terminator -- see bb_json_render_impl()). Proven by
// test_emit_status_json_render_buf_headroom (test_bb_ota_check_emit.c),
// which drives this exact path (bb_ota_check_now() -> mark_check_on_apply()
// -> emit_status_json()) with a max-length fixture; the measured render
// there is 739 bytes (a few bytes under the 746 derived above, since that
// fixture's last_check_ts hits the tighter truncated-division width rather
// than the full int64 type bound, and enabled renders "true" by default).
#define BB_OTA_CHECK_RENDER_BUF_BYTES 1024

bb_err_t bb_ota_check_emit_status_json(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

    if (!s_initialized) {
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "not initialized");
        bb_http_resp_json_obj_end(&err_obj);
        return BB_OK;
    }

    // DO NOT DELETE this refresh as "redundant with bb_data_render()'s
    // per-render gather" -- it is NOT redundant for this key. VERIFIED
    // against bb_ota_check_gather()'s actual body (bb_ota_check_wire.c)
    // before this cutover: that gather hook is a PURE PASS-THROUGH over
    // bb_cache_get_raw() -- it does not itself read live s_status/the
    // bb_settings opt-out flag. Without this refresh, a GET would freeze at
    // whatever was last published via publish_state() (periodic check /
    // publish_initial), missing a since-mark_check_on_apply() transition or
    // a runtime bb_settings_update_check_enabled_set() toggle with no check
    // having run since. Preserves ts from the last SSE publish (this
    // handler has no independent notion of a publish time).
    {
        bb_ota_check_status_t st;
        bb_ota_check_get_status(&st);
        bb_ota_check_snap_t snap;
        memset(&snap, 0, sizeof(snap));
        bb_strlcpy(snap.current,      st.current,      sizeof(snap.current));
        bb_strlcpy(snap.latest,       st.latest,       sizeof(snap.latest));
        bb_strlcpy(snap.download_url, st.download_url, sizeof(snap.download_url));
        snap.available     = st.available;
        snap.ts            = s_last_publish_ts;  // preserved from last publish_state
        snap.last_check_ok = st.last_check_ok;
        snap.enabled       = st.enabled;
        bb_strlcpy(snap.outcome, outcome_str(st.outcome), sizeof(snap.outcome));
        snap.last_check_ts = (st.last_check_us != 0)
                             ? (int64_t)(st.last_check_us / 1000000) : 0;
        bb_cache_update(&(bb_cache_update_t){ .key = BB_OTA_CHECK_TOPIC, .snap = &snap });
    }

    char   scratch[sizeof(bb_ota_check_snap_t)];
    char   data_buf[BB_OTA_CHECK_RENDER_BUF_BYTES];
    size_t data_len = 0;
    bb_data_render_req_t render_req = {
        .fmt         = BB_FORMAT_JSON,
        .key         = BB_OTA_CHECK_TOPIC,
        .query       = NULL,
        .scratch     = scratch,
        .scratch_cap = sizeof(scratch),
        .buf         = data_buf,
        .buf_cap     = sizeof(data_buf),
        .out_len     = &data_len,
    };
    bb_err_t err = bb_data_render(&render_req);
    if (err != BB_OK) {
        // Unbound (BB_ERR_NOT_FOUND, e.g. bb_ota_check_bind() failed at init
        // due to a full bb_data table) or an over-length render -- either
        // way this is a producer-side condition, not a client error.
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "render failed");
        bb_http_resp_json_obj_end(&err_obj);
        return err;
    }

    bb_http_json_obj_stream_t obj;
    err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_int(&obj, "ts_ms", (int64_t)bb_clock_now_ms64());
    bb_http_resp_json_obj_set_raw(&obj, "data", data_buf, data_len);

    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_OTA_CHECK_TESTING
void bb_ota_check_reset_for_test(void)
{
    pthread_mutex_lock(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.outcome = BB_OTA_CHECK_OUTCOME_UNKNOWN;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_url[0] = '\0';
    s_firmware_board[0] = '\0';
    s_parser = bb_release_manifest_parse_github;
    s_pause_hook = NULL;
    s_resume_hook = NULL;
    s_first_check_done = false;
    s_initialized = false;
    s_last_publish_ts = 0;
    pthread_mutex_unlock(&s_lock);
    // Restore default: update check enabled (mirrors the field's has_default=true).
    bb_settings_update_check_enabled_set(true);
#ifndef ESP_PLATFORM
    // Reset the in-flight guard so each test starts clean.
    atomic_store(&s_in_flight, false);
#endif
}

// See bb_ota_check_internal.h doc comment.
void bb_ota_check_set_current_for_test(const char *version)
{
    pthread_mutex_lock(&s_lock);
    bb_strlcpy(s_status.current, version, sizeof(s_status.current));
    pthread_mutex_unlock(&s_lock);
}

// See bb_ota_check_internal.h doc comment.
void bb_ota_check_set_ts_for_test(int64_t publish_ts_s, int64_t last_check_us)
{
    pthread_mutex_lock(&s_lock);
    s_last_publish_ts = publish_ts_s;
    s_status.last_check_us = last_check_us;
    pthread_mutex_unlock(&s_lock);
}

#ifndef ESP_PLATFORM
// Host/Arduino stub for the OTA exclusive-slot claim.
// On ESP-IDF the real implementation lives in bb_ota_check_espidf.c (backed
// by a bb_claim_t that the upd_check worker also acquires). On host there is no
// upd_check worker task, so the stub provides the same arbiter semantics via a
// portable bb_claim_t so host tests can verify claim acquire/release behaviour.
#include "bb_claim.h"
static bb_claim_t s_ota_claim_host = BB_CLAIM_INIT;

bb_err_t bb_ota_check_ota_claim_acquire(const char *id)
{
    return bb_claim_acquire(&s_ota_claim_host, id);
}

void bb_ota_check_ota_claim_release(const char *id)
{
    bb_claim_release(&s_ota_claim_host, id);
}

#ifdef BB_OTA_CHECK_TESTING
void bb_ota_check_ota_claim_reset(void)
{
    bb_claim_reset(&s_ota_claim_host);
}

const char *bb_ota_check_ota_claim_holder_for_test(void)
{
    return bb_claim_holder(&s_ota_claim_host);
}
#endif // BB_OTA_CHECK_TESTING
#endif // !ESP_PLATFORM

#ifndef ESP_PLATFORM
// Inject the in-flight state for testing the concurrency guard on host.
void bb_ota_check_set_in_flight_for_test(bool in_flight)
{
    atomic_store(&s_in_flight, in_flight);
}

bool bb_ota_check_get_in_flight_for_test(void)
{
    return atomic_load(&s_in_flight);
}
#else
// On ESP-IDF, the in-flight flag lives in bb_ota_check_espidf.c.
// Provide forward declarations (defined there) so the internal header compiles.
void bb_ota_check_set_in_flight_for_test(bool in_flight);
bool bb_ota_check_get_in_flight_for_test(void);
#endif
#endif
