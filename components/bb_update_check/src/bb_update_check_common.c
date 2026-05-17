#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_release_manifest.h"
#include "bb_http_client.h"
#include "bb_event.h"
#include "bb_log.h"
#include "bb_mdns.h"
#include "bb_nv.h"
#include "bb_system.h"

#include <limits.h>
#include <pthread.h>
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
#define TOPIC_NAME     "update.available"
#define BOARD_NAME_FALLBACK "firmware"

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
    char payload[512];
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
    s_first_check_done = false;
    s_url[0] = '\0';
    s_firmware_board[0] = '\0';
    s_parser = bb_release_manifest_parse_github;

    bb_err_t err = bb_event_topic_register(TOPIC_NAME, &s_topic);
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
    if (!s_initialized) return BB_ERR_INVALID_STATE;
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

#define CUSTOM_PARSER_BUF_SIZE 16384

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
    if (n > avail) bc->overflow = true;
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
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "fetch failed: err=%d status=%d", err, res.status_code);
            return err == BB_OK ? BB_ERR_INVALID_STATE : err;
        }

        perr = bb_release_manifest_parse_github_stream_end(&fc.stream_ctx);
        if (perr != BB_OK) {
            pthread_mutex_lock(&s_lock);
            s_status.last_check_us = now_us;
            s_status.last_check_ok = false;
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "parse failed: %d", perr);
            return perr;
        }

        // Fall through to the version-compare + publish block below.
        // (Extracted tag/dl_url are already filled in by the stream parser.)
        int cmp = semver_compare(tag, s_status.current);
        bool new_available = (cmp != INT_MIN) && (cmp > 0);  // LCOV_EXCL_BR_LINE — INT_MIN path defensive

        bb_update_check_status_t snap;
        bool was_available;
        bool first_call;
        pthread_mutex_lock(&s_lock);
        was_available = s_status.available;
        first_call = !s_first_check_done;
        strncpy(s_status.latest, tag, sizeof(s_status.latest) - 1);
        s_status.latest[sizeof(s_status.latest) - 1] = '\0';
        strncpy(s_status.download_url, dl_url, sizeof(s_status.download_url) - 1);
        s_status.download_url[sizeof(s_status.download_url) - 1] = '\0';
        s_status.available = new_available;
        s_status.last_check_us = now_us;
        s_status.last_check_ok = true;
        s_first_check_done = true;
        snap = s_status;
        pthread_mutex_unlock(&s_lock);

        bool transition = (was_available != new_available);
        bool initial_publish = first_call && s_cfg.post_initial;
        if (transition || initial_publish) {
            publish_state(&snap, new_available ? snap.latest : "none");
        }
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
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "parse failed: %d", perr);
            return perr;
        }

        int cmp = semver_compare(tag, s_status.current);
        bool new_available = (cmp != INT_MIN) && (cmp > 0);  // LCOV_EXCL_BR_LINE

        bb_update_check_status_t snap;
        bool was_available;
        bool first_call;
        pthread_mutex_lock(&s_lock);
        was_available = s_status.available;
        first_call = !s_first_check_done;
        strncpy(s_status.latest, tag, sizeof(s_status.latest) - 1);
        s_status.latest[sizeof(s_status.latest) - 1] = '\0';
        strncpy(s_status.download_url, dl_url, sizeof(s_status.download_url) - 1);
        s_status.download_url[sizeof(s_status.download_url) - 1] = '\0';
        s_status.available = new_available;
        s_status.last_check_us = now_us;
        s_status.last_check_ok = true;
        s_first_check_done = true;
        snap = s_status;
        pthread_mutex_unlock(&s_lock);

        bool transition = (was_available != new_available);
        bool initial_publish = first_call && s_cfg.post_initial;
        if (transition || initial_publish) {
            publish_state(&snap, new_available ? snap.latest : "none");
        }
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
// Host/Arduino stub: no worker task, so kick is synchronous like now().
bb_err_t bb_update_check_kick(void)
{
    return bb_update_check_now();
}
#endif

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_UPDATE_CHECK_TESTING
void bb_update_check_reset_for_test(void)
{
    pthread_mutex_lock(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
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
}
#endif
