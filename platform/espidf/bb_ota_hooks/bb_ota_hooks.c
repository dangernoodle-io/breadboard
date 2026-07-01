#include "bb_ota_hooks.h"
#include "bb_log.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_openapi.h"
#include "bb_init.h"
#endif

// ---------------------------------------------------------------------------
// File statics
// ---------------------------------------------------------------------------

static bb_http_pause_cb_t       s_pause_cb;
static bb_http_resume_cb_t      s_resume_cb;
static bb_ota_skip_check_cb_t   s_skip_check_cb;
static bb_ota_progress_cb_t     s_progress_cb;
static bb_ota_phase_t           s_last_phase = BB_OTA_PHASE_FAIL;
static int                      s_last_pct;

#ifdef ESP_PLATFORM
static bb_event_topic_t s_ota_progress_topic = NULL;
#endif

static const char *TAG = "bb_ota";

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------

void bb_ota_set_hooks(bb_http_pause_cb_t pause, bb_http_resume_cb_t resume)
{
    s_pause_cb  = pause;
    s_resume_cb = resume;
}

void bb_ota_set_progress_cb(bb_ota_progress_cb_t cb)
{
    s_progress_cb = cb;
}

void bb_ota_set_skip_check_cb(bb_ota_skip_check_cb_t cb)
{
    s_skip_check_cb = cb;
}

// ---------------------------------------------------------------------------
// Invokers
// ---------------------------------------------------------------------------

bool bb_ota_pause(void)
{
    return s_pause_cb ? s_pause_cb() : false;
}

bool bb_ota_has_pause_hook(void)
{
    return s_pause_cb != NULL;
}

void bb_ota_resume(void)
{
    if (s_resume_cb) s_resume_cb();
}

bool bb_ota_skip_check(void)
{
    return s_skip_check_cb && s_skip_check_cb();
}

void bb_ota_last_progress(bb_ota_phase_t *phase, int *pct)
{
    if (phase) *phase = s_last_phase;
    if (pct)   *pct   = s_last_pct;
}

// ---------------------------------------------------------------------------
// Pure JSON builder (host-testable — outside ESP_PLATFORM guard)
// ---------------------------------------------------------------------------

int bb_ota_progress_json(char *buf, size_t sz, const char *via, int state, int pct)
{
    static const char *names[] = {"start", "progress", "success", "fail"};
    const char *s = (state >= 0 && state <= 3) ? names[state] : "unknown";
    int n = snprintf(buf, sz, "{\"via\":\"%s\",\"state\":\"%s\",\"pct\":%d}",
                     via ? via : "", s, pct);
    if (n <= 0 || (size_t)n >= sz) return 0;
    return n;
}

// ---------------------------------------------------------------------------
// Progress emitter
// ---------------------------------------------------------------------------

void bb_ota_emit_progress(const char *via, bb_ota_phase_t phase, int pct)
{
    if (s_progress_cb) s_progress_cb(phase, pct);

    switch (phase) {
        case BB_OTA_PHASE_START:    bb_log_i(TAG, "OTA %s: starting", via);  break;
        // PROGRESS: the per-% serial line is logged by each caller with its own
        // richer detail (byte counts) — don't duplicate it here.
        case BB_OTA_PHASE_PROGRESS:                                          break;
        case BB_OTA_PHASE_SUCCESS:  bb_log_i(TAG, "OTA %s: complete", via);  break;
        case BB_OTA_PHASE_FAIL:     bb_log_w(TAG, "OTA %s: failed", via);    break;
    }

    s_last_phase = phase;
    s_last_pct   = pct;

#ifdef ESP_PLATFORM
    if (s_ota_progress_topic) {
        int state = (phase == BB_OTA_PHASE_START)    ? 0 :
                    (phase == BB_OTA_PHASE_PROGRESS)  ? 1 :
                    (phase == BB_OTA_PHASE_SUCCESS)   ? 2 : 3;
        char p[96];
        int n = bb_ota_progress_json(p, sizeof(p), via, state, pct);
        if (n > 0) bb_event_post(s_ota_progress_topic, pct, p, (size_t)n);
    }
#endif
}

// ---------------------------------------------------------------------------
// ESP-IDF regular-tier init — attach ota.progress topic
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM

static const char k_ota_progress_schema[] =
    "{\"title\":\"OtaProgress\",\"x-sse-topic\":\"ota.progress\",\"type\":\"object\","
    "\"properties\":{"
    "\"via\":{\"type\":\"string\"},"
    "\"state\":{\"type\":\"string\","
    "\"enum\":[\"start\",\"progress\",\"success\",\"fail\",\"unknown\"]},"
    "\"pct\":{\"type\":\"integer\"}},"
    "\"required\":[\"via\",\"state\",\"pct\"]}";

static bb_err_t bb_ota_hooks_init(bb_http_handle_t server)
{
    (void)server;
#if defined(CONFIG_BB_OTA_HOOKS_AUTO_ATTACH) && CONFIG_BB_OTA_HOOKS_AUTO_ATTACH
    if (bb_event_topic_register("ota.progress", &s_ota_progress_topic) == BB_OK) {
        bb_openapi_register_topic_schema("ota.progress", k_ota_progress_schema, "OtaProgress");
        bb_err_t ae = bb_event_routes_attach_ex("ota.progress", false); // non-retained
        if (ae != BB_OK) bb_log_w(TAG, "auto-attach failed for 'ota.progress': %d", ae);
    }
#endif
    return BB_OK;
}

#if CONFIG_BB_OTA_HOOKS_AUTOREGISTER
/* order 4: after bb_event_routes_init (order 0) sets s_cfg.initialized so the
 * attach succeeds — mirrors bb_update_check. */
BB_INIT_REGISTER_N(bb_ota_hooks, bb_ota_hooks_init, 4);
#endif

#endif // ESP_PLATFORM
