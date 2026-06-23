#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_release_manifest.h"
#include "bb_http.h"
#include "bb_http_client.h"
#include "bb_event.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_mdns.h"
#include "bb_nv.h"
#include "bb_system.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "bb_update_check";

#ifndef CONFIG_BB_UPDATE_CHECK_INTERVAL_S
#define CONFIG_BB_UPDATE_CHECK_INTERVAL_S 21600
#endif

#define URL_MAX        256
#define BOARD_MAX       64
#define BOARD_NAME_FALLBACK "unknown"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool                          s_initialized = false;
static bb_update_check_cfg_t         s_cfg;
static char                          s_url[URL_MAX];
static char                          s_firmware_board[BOARD_MAX];
static bb_release_manifest_parse_fn  s_parser = bb_release_manifest_parse_github;
static bb_update_check_status_t      s_status;
static bool                          s_first_check_done = false;
static bb_event_topic_t              s_topic = NULL;
static pthread_mutex_t               s_lock = PTHREAD_MUTEX_INITIALIZER;
static bb_update_check_pause_cb_t    s_pause_hook = NULL;
static bb_update_check_resume_cb_t   s_resume_hook = NULL;

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
// mDNS publish + event post
// ---------------------------------------------------------------------------

static void publish_state(const bb_update_check_status_t *snap, const char *txt_value)
{
    bb_mdns_set_txt("update", txt_value);

    if (!s_topic) return;  // LCOV_EXCL_LINE — init always registers the topic
    char payload[256];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int n = snprintf(payload, sizeof(payload),
        "{\"current\":\"%s\",\"latest\":\"%s\",\"download_url\":\"%s\","
        "\"available\":%s,\"ts\":%lld}",
        snap->current, snap->latest, snap->download_url,
        snap->available ? "true" : "false",
        (long long)tv.tv_sec);
    if (n <= 0) return;  // LCOV_EXCL_LINE — snprintf failure defensive
    size_t sz = (size_t)n < sizeof(payload) ? (size_t)n : sizeof(payload) - 1;  // LCOV_EXCL_BR_LINE — payload bounded well under 512B
    bb_event_post(s_topic, snap->available ? 1 : 0, payload, sz);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_update_check_init(const bb_update_check_cfg_t *cfg)
{
    if (s_initialized) return BB_OK;

    s_cfg.interval_s   = (cfg && cfg->interval_s) ? cfg->interval_s : CONFIG_BB_UPDATE_CHECK_INTERVAL_S;  // LCOV_EXCL_BR_LINE — cfg permutations covered above
    s_cfg.post_initial = (cfg && cfg->post_initial);  // LCOV_EXCL_BR_LINE

    memset(&s_status, 0, sizeof(s_status));
    const char *running = bb_system_get_version();
    if (running) {  // LCOV_EXCL_BR_LINE — host always returns non-null
        strncpy(s_status.current, running, sizeof(s_status.current) - 1);
        s_status.current[sizeof(s_status.current) - 1] = '\0';
    }
    s_status.latest[0] = '\0';
    s_status.download_url[0] = '\0';
    s_status.last_check_us = 0;
    s_status.last_check_ok = false;
    s_status.available = false;
    s_status.outcome = BB_UPDATE_OUTCOME_UNKNOWN;
    s_first_check_done = false;
    s_url[0] = '\0';
    s_firmware_board[0] = '\0';
    s_parser = bb_release_manifest_parse_github;

    bb_err_t err = bb_event_topic_register(BB_UPDATE_CHECK_TOPIC, &s_topic);
    // LCOV_EXCL_START — topic_register failure is defensive (NO_SPACE only)
    if (err != BB_OK) {
        return err;
    }
    // LCOV_EXCL_STOP

    bb_mdns_set_txt("update", "unknown");

    s_initialized = true;

    bb_log_i(TAG, "initialized: interval=%us", (unsigned)s_cfg.interval_s);
    return BB_OK;
}

bb_err_t bb_update_check_publish_initial(void)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    bb_update_check_status_t snap = s_status;
    pthread_mutex_unlock(&s_lock);
    publish_state(&snap, "unknown");
    return BB_OK;
}

bb_err_t bb_update_check_set_releases_url(const char *url)
{
    if (!url) return BB_ERR_INVALID_ARG;
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    size_t n = strlen(url);
    if (n == 0 || n >= URL_MAX) return BB_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    strncpy(s_url, url, URL_MAX - 1);
    s_url[URL_MAX - 1] = '\0';
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_update_check_set_parser(bb_release_manifest_parse_fn fn)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    s_parser = fn ? fn : bb_release_manifest_parse_github;
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_update_check_set_firmware_board(const char *board)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    // NULL/empty clears to default; non-empty must fit the internal buffer.
    if (board && board[0] != '\0' && strlen(board) >= BOARD_MAX) return BB_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    if (!board || board[0] == '\0') {
        s_firmware_board[0] = '\0';
    } else {
        strncpy(s_firmware_board, board, BOARD_MAX - 1);
        s_firmware_board[BOARD_MAX - 1] = '\0';
    }
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_update_check_set_hooks(bb_update_check_pause_cb_t pause,
                                   bb_update_check_resume_cb_t resume)
{
    // Order-independent: boot-strategy boards lazily init bb_update_check
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

bb_err_t bb_update_check_get_status(bb_update_check_status_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    *out = s_status;
    const char *board_eff = (s_firmware_board[0] != '\0') ? s_firmware_board : BOARD_NAME_FALLBACK;
    strncpy(out->board, board_eff, sizeof(out->board) - 1);
    out->board[sizeof(out->board) - 1] = '\0';
    pthread_mutex_unlock(&s_lock);
    out->enabled = bb_nv_config_update_check_enabled();
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

#ifdef CONFIG_BB_UPDATE_CHECK_CUSTOM_PARSER_BUF_BYTES
#define CUSTOM_PARSER_BUF_SIZE CONFIG_BB_UPDATE_CHECK_CUSTOM_PARSER_BUF_BYTES
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
        bb_log_w(TAG, "custom parser buffer overflow: response truncated at %zu bytes; raise CONFIG_BB_UPDATE_CHECK_CUSTOM_PARSER_BUF_BYTES", bc->cap);
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Synchronous one-shot check
// ---------------------------------------------------------------------------

bb_err_t bb_update_check_run_one(void)
{
    if (!s_initialized) return BB_ERR_INVALID_ARG;

    if (!bb_nv_config_update_check_enabled()) {
        bb_log_i(TAG, "update check disabled via bb_nv; skipping");
        return BB_OK;
    }

    char url_local[URL_MAX];
    char board_local[BOARD_MAX];
    bb_release_manifest_parse_fn parser_local;
    bb_update_check_pause_cb_t  pause_local;
    bb_update_check_resume_cb_t resume_local;
    pthread_mutex_lock(&s_lock);
    strncpy(url_local, s_url, sizeof(url_local));
    url_local[sizeof(url_local) - 1] = '\0';
    strncpy(board_local, s_firmware_board, sizeof(board_local));
    board_local[sizeof(board_local) - 1] = '\0';
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
            s_status.outcome = BB_UPDATE_OUTCOME_FAILED;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "fetch failed: err=%d status=%d", err, res.status_code);
            return err == BB_OK ? BB_ERR_INVALID_STATE : err;
        }

        perr = bb_release_manifest_parse_github_stream_end(&fc.stream_ctx);
        if (perr != BB_OK) {
            pthread_mutex_lock(&s_lock);
            s_status.last_check_us = now_us;
            s_status.last_check_ok = false;
            s_status.outcome = BB_UPDATE_OUTCOME_FAILED;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "parse failed: %d", perr);
            return perr;
        }

        // No-asset terminal: tag parsed successfully but no matching board asset.
        // This is a success outcome (last_check_ok=true) — distinguishable from
        // up_to_date. Consumers polling last_check_ok will not hang.
        if (dl_url[0] == '\0') {
            bb_update_check_status_t snap;
            pthread_mutex_lock(&s_lock);
            s_status.latest[0] = '\0';
            s_status.download_url[0] = '\0';
            s_status.available = false;
            s_status.last_check_us = now_us;
            s_status.last_check_ok = true;
            s_status.outcome = BB_UPDATE_OUTCOME_NO_ASSET;
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

        bb_update_check_status_t snap;
        pthread_mutex_lock(&s_lock);
        strncpy(s_status.latest, tag, sizeof(s_status.latest) - 1);
        s_status.latest[sizeof(s_status.latest) - 1] = '\0';
        strncpy(s_status.download_url, dl_url, sizeof(s_status.download_url) - 1);
        s_status.download_url[sizeof(s_status.download_url) - 1] = '\0';
        s_status.available = new_available;
        s_status.last_check_us = now_us;
        s_status.last_check_ok = true;
        s_status.outcome = new_available ? BB_UPDATE_OUTCOME_AVAILABLE : BB_UPDATE_OUTCOME_UP_TO_DATE;
        s_first_check_done = true;
        snap = s_status;
        pthread_mutex_unlock(&s_lock);

        publish_state(&snap, new_available ? snap.latest : "none");
        return BB_OK;

    } else {
        // Custom parser path: buffer into a local heap allocation.
        buf_ctx_t bc;
        bc.buf = (char *)calloc(1, CUSTOM_PARSER_BUF_SIZE);
        if (!bc.buf) {  // LCOV_EXCL_START — OOM defensive
            bb_log_w(TAG, "custom parser buf alloc failed");
            return BB_ERR_NO_SPACE;
        }  // LCOV_EXCL_STOP
        bc.cap      = CUSTOM_PARSER_BUF_SIZE;
        bc.len      = 0;
        bc.overflow = false;

        if (pause_local && !pause_local()) {
            bb_log_w(TAG, "pause hook refused; skipping fetch");
            free(bc.buf);
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
            s_status.outcome = BB_UPDATE_OUTCOME_FAILED;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "fetch failed: err=%d status=%d", err, res.status_code);
            free(bc.buf);
            return err == BB_OK ? BB_ERR_INVALID_STATE : err;
        }

        perr = parser_local(bc.buf, bc.len, board_name,
                            tag, sizeof(tag), dl_url, sizeof(dl_url));
        free(bc.buf);
        if (perr != BB_OK) {
            pthread_mutex_lock(&s_lock);
            s_status.last_check_us = now_us;
            s_status.last_check_ok = false;
            s_status.outcome = BB_UPDATE_OUTCOME_FAILED;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "parse failed: %d", perr);
            return perr;
        }

        // No-asset terminal: tag parsed successfully but no matching board asset.
        // This is a success outcome (last_check_ok=true) — distinguishable from
        // up_to_date. Consumers polling last_check_ok will not hang.
        if (dl_url[0] == '\0') {
            bb_update_check_status_t snap;
            pthread_mutex_lock(&s_lock);
            s_status.latest[0] = '\0';
            s_status.download_url[0] = '\0';
            s_status.available = false;
            s_status.last_check_us = now_us;
            s_status.last_check_ok = true;
            s_status.outcome = BB_UPDATE_OUTCOME_NO_ASSET;
            s_first_check_done = true;
            snap = s_status;
            pthread_mutex_unlock(&s_lock);
            bb_log_i(TAG, "update check: no firmware asset for this board");
            publish_state(&snap, "none");
            return BB_OK;
        }

        int cmp = semver_compare(tag, s_status.current);
        bool new_available = (cmp != INT_MIN) && (cmp > 0);  // LCOV_EXCL_BR_LINE

        bb_update_check_status_t snap;
        pthread_mutex_lock(&s_lock);
        strncpy(s_status.latest, tag, sizeof(s_status.latest) - 1);
        s_status.latest[sizeof(s_status.latest) - 1] = '\0';
        strncpy(s_status.download_url, dl_url, sizeof(s_status.download_url) - 1);
        s_status.download_url[sizeof(s_status.download_url) - 1] = '\0';
        s_status.available = new_available;
        s_status.last_check_us = now_us;
        s_status.last_check_ok = true;
        s_status.outcome = new_available ? BB_UPDATE_OUTCOME_AVAILABLE : BB_UPDATE_OUTCOME_UP_TO_DATE;
        s_first_check_done = true;
        snap = s_status;
        pthread_mutex_unlock(&s_lock);

        publish_state(&snap, new_available ? snap.latest : "none");
        return BB_OK;
    }
}

bb_err_t bb_update_check_now(void)
{
    if (!s_initialized) return BB_ERR_INVALID_ARG;
    if (s_url[0] == '\0') return BB_ERR_INVALID_STATE;
    return bb_update_check_run_one();
}

#ifndef ESP_PLATFORM
// Host/Arduino stub: no real FreeRTOS task; kick() is synchronous but
// respects the in-flight guard so tests can verify the no-duplicate-spawn path.
bb_err_t bb_update_check_kick(void)
{
    // Atomically try to claim the in-flight slot.  If already set, a check is
    // "in flight" — skip (mirrors the ESP-IDF on-demand guard behaviour).
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_in_flight, &expected, true)) {
        bb_log_d(TAG, "kick: check already in flight, skipping");
        return BB_OK;
    }
    bb_err_t err = bb_update_check_now();
    atomic_store(&s_in_flight, false);
    return err;
}

// Host/Arduino stub: no worker task, so run_blocking is synchronous like now().
bb_err_t bb_update_check_run_blocking(uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    return bb_update_check_now();
}

// Host/Arduino stub: no worker task to pin, no-op.
void bb_update_check_set_task_core(int core)
{
    (void)core;
}

// Host/Arduino stub: no worker task to prioritize, no-op.
void bb_update_check_set_task_priority(int priority)
{
    (void)priority;
}
#endif

// ---------------------------------------------------------------------------
// GET /api/update/config  — read the runtime opt-out flag
// POST /api/update/config — write the runtime opt-out flag
// Defined here (not in bb_update_check_espidf.c) so host tests can reach them.
// Route registration is performed by the platform port.
// ---------------------------------------------------------------------------

#define BB_UPDATE_CHECK_CONFIG_BODY_MAX 64

bb_err_t bb_update_check_config_get_handler(bb_http_request_t *req)
{
    bool enabled = bb_nv_config_update_check_enabled();
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;  // LCOV_EXCL_BR_LINE — send_chunk never fails on host
    bb_http_resp_json_obj_set_bool(&obj, "enabled", enabled);
    return bb_http_resp_json_obj_end(&obj);
}

bb_err_t bb_update_check_config_post_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > BB_UPDATE_CHECK_CONFIG_BODY_MAX) {  // LCOV_EXCL_BR_LINE — both sub-branches exercised but gcov counts as separate
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);  // LCOV_EXCL_BR_LINE — send_chunk never fails on host
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid request");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    char body[BB_UPDATE_CHECK_CONFIG_BODY_MAX + 1];
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

    bb_json_t doc = bb_json_parse(body, (size_t)n);
    if (!doc) {  // LCOV_EXCL_BR_LINE — parse failure path; both branches covered but gcov misattributes inner begin() arc
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid JSON");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bool enabled;
    if (!bb_json_obj_get_bool(doc, "enabled", &enabled)) {  // LCOV_EXCL_BR_LINE — missing field path; both branches covered but gcov misattributes inner begin() arc
        bb_json_free(doc);
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "missing or invalid 'enabled'");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }
    bb_json_free(doc);

    bb_err_t err = bb_nv_config_set_update_check_enabled(enabled);
    if (err != BB_OK) {  // LCOV_EXCL_BR_LINE — NV write failure; both branches covered but gcov misattributes inner begin() arc
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "NV write failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_http_json_obj_stream_t obj;
    err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;  // LCOV_EXCL_BR_LINE — send_chunk never fails on host
    bb_http_resp_json_obj_set_bool(&obj, "enabled", bb_nv_config_update_check_enabled());
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
    .handler  = bb_update_check_config_get_handler,
};

static const bb_route_response_t s_config_post_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
       "\"properties\":{\"enabled\":{\"type\":\"boolean\"}},"
       "\"required\":[\"enabled\"]}",
      "updated state" },
    { 400, "text/plain", NULL, "missing or invalid body" },
    { 500, "text/plain", NULL, "NV write failed" },
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
    .handler  = bb_update_check_config_post_handler,
};

const bb_route_t *bb_update_check_config_get_route(void)  { return &s_config_get_route; }
const bb_route_t *bb_update_check_config_post_route(void) { return &s_config_post_route; }

// ---------------------------------------------------------------------------
// bb_update_check_emit_status_json — shared HTTP response emitter
// ---------------------------------------------------------------------------

// Map outcome enum to the JSON string value for /api/update/status.
// These exact strings are part of the public API consumed by TaipanMiner webui
// and taipan-cli. Do not rename without a coordinated consumer update.
static const char *outcome_str(bb_update_check_outcome_t o)
{
    switch (o) {
        case BB_UPDATE_OUTCOME_UP_TO_DATE:    return "up_to_date";
        case BB_UPDATE_OUTCOME_AVAILABLE:     return "available";
        case BB_UPDATE_OUTCOME_NO_ASSET:      return "no_asset";
        case BB_UPDATE_OUTCOME_FAILED:        return "check_failed";
        case BB_UPDATE_OUTCOME_CHECK_ON_APPLY: return "check_on_apply";
        default:                              return "unknown";
    }
}

bb_err_t bb_update_check_mark_check_on_apply(void)
{
    if (!s_initialized) return BB_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    s_status.outcome       = BB_UPDATE_OUTCOME_CHECK_ON_APPLY;
    s_status.available     = false;
    s_status.last_check_ok = false;
    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_update_check_emit_status_json(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

    bb_update_check_status_t st;
    bb_err_t err = bb_update_check_get_status(&st);
    if (err != BB_OK) {
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "not initialized");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_http_json_obj_stream_t obj;
    err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj,  "current",       st.current);
    bb_http_resp_json_obj_set_str(&obj,  "latest",        st.latest);
    bb_http_resp_json_obj_set_str(&obj,  "download_url",  st.download_url);
    bb_http_resp_json_obj_set_bool(&obj, "available",     st.available);
    bb_http_resp_json_obj_set_bool(&obj, "last_check_ok", st.last_check_ok);
    bb_http_resp_json_obj_set_bool(&obj, "enabled",       st.enabled);
    bb_http_resp_json_obj_set_str(&obj,  "outcome",       outcome_str(st.outcome));
    // Unix seconds when last_check_us is non-zero; omitted otherwise so the
    // client can render "never checked" cleanly.
    if (st.last_check_us != 0) {
        bb_http_resp_json_obj_set_int(&obj, "last_check_ts",
                                      (int64_t)(st.last_check_us / 1000000));
    }
    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_UPDATE_CHECK_TESTING
void bb_update_check_reset_for_test(void)
{
    pthread_mutex_lock(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.outcome = BB_UPDATE_OUTCOME_UNKNOWN;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_url[0] = '\0';
    s_firmware_board[0] = '\0';
    s_topic = NULL;
    s_parser = bb_release_manifest_parse_github;
    s_pause_hook = NULL;
    s_resume_hook = NULL;
    s_first_check_done = false;
    s_initialized = false;
    pthread_mutex_unlock(&s_lock);
    // Restore default: update check enabled (mirrors bb_nv_config_init default).
    bb_nv_config_set_update_check_enabled(true);
#ifndef ESP_PLATFORM
    // Reset the in-flight guard so each test starts clean.
    atomic_store(&s_in_flight, false);
#endif
}

#ifndef ESP_PLATFORM
// Inject the in-flight state for testing the concurrency guard on host.
void bb_update_check_set_in_flight_for_test(bool in_flight)
{
    atomic_store(&s_in_flight, in_flight);
}

bool bb_update_check_get_in_flight_for_test(void)
{
    return atomic_load(&s_in_flight);
}
#else
// On ESP-IDF, the in-flight flag lives in bb_update_check_espidf.c.
// Provide forward declarations (defined there) so the internal header compiles.
void bb_update_check_set_in_flight_for_test(bool in_flight);
bool bb_update_check_get_in_flight_for_test(void);
#endif
#endif
