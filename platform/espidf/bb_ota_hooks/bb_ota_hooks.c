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

// Pause/resume are registered as a pair (one bb_ota_set_hooks() call = one
// registrant); dispatched in registration order for both pause and resume so
// resume iterates the same set pause used.
typedef struct {
    bb_http_pause_cb_t  pause;
    bb_http_resume_cb_t resume;
} bb_ota_hook_pair_t;

static bb_ota_hook_pair_t     s_pairs[BB_OTA_HOOKS_MAX];
static size_t                 s_pair_count;

static bb_ota_progress_cb_t   s_progress_cbs[BB_OTA_HOOKS_MAX];
static size_t                 s_progress_count;

static bb_ota_skip_check_cb_t s_skip_cbs[BB_OTA_HOOKS_MAX];
static size_t                 s_skip_count;

static bb_ota_phase_t           s_last_phase = BB_OTA_PHASE_FAIL;
static int                      s_last_pct;

#ifdef ESP_PLATFORM
static bb_event_topic_t s_ota_progress_topic = NULL;
#endif

static const char *TAG = "bb_ota";

// ---------------------------------------------------------------------------
// Setters (append semantics — a 2nd registrant is added to the list, not
// swapped in over the 1st)
// ---------------------------------------------------------------------------

void bb_ota_set_hooks(bb_http_pause_cb_t pause, bb_http_resume_cb_t resume)
{
    if (s_pair_count >= BB_OTA_HOOKS_MAX) {
        bb_log_w(TAG, "pause/resume hook registry full (cap %d); dropping registrant",
                 BB_OTA_HOOKS_MAX);
        return;
    }
    if (s_pair_count == BB_OTA_HOOKS_MAX - 1) {
        bb_log_w(TAG, "pause/resume hook registry at high-watermark (%d/%d)",
                 (int)s_pair_count + 1, BB_OTA_HOOKS_MAX);
    }
    s_pairs[s_pair_count].pause  = pause;
    s_pairs[s_pair_count].resume = resume;
    s_pair_count++;
}

void bb_ota_set_progress_cb(bb_ota_progress_cb_t cb)
{
    if (s_progress_count >= BB_OTA_HOOKS_MAX) {
        bb_log_w(TAG, "progress hook registry full (cap %d); dropping registrant",
                 BB_OTA_HOOKS_MAX);
        return;
    }
    if (s_progress_count == BB_OTA_HOOKS_MAX - 1) {
        bb_log_w(TAG, "progress hook registry at high-watermark (%d/%d)",
                 (int)s_progress_count + 1, BB_OTA_HOOKS_MAX);
    }
    s_progress_cbs[s_progress_count++] = cb;
}

void bb_ota_set_skip_check_cb(bb_ota_skip_check_cb_t cb)
{
    if (s_skip_count >= BB_OTA_HOOKS_MAX) {
        bb_log_w(TAG, "skip-check hook registry full (cap %d); dropping registrant",
                 BB_OTA_HOOKS_MAX);
        return;
    }
    if (s_skip_count == BB_OTA_HOOKS_MAX - 1) {
        bb_log_w(TAG, "skip-check hook registry at high-watermark (%d/%d)",
                 (int)s_skip_count + 1, BB_OTA_HOOKS_MAX);
    }
    s_skip_cbs[s_skip_count++] = cb;
}

// ---------------------------------------------------------------------------
// Observability
// ---------------------------------------------------------------------------

size_t bb_ota_hooks_pause_count(void)
{
    return s_pair_count;
}

size_t bb_ota_hooks_progress_count(void)
{
    return s_progress_count;
}

size_t bb_ota_hooks_skip_check_count(void)
{
    return s_skip_count;
}

// ---------------------------------------------------------------------------
// Invokers
// ---------------------------------------------------------------------------

// Combine semantics: OR — pause is considered engaged if ANY registered pause
// hook returns true (at least one consumer needs the OTA to wait for it).
bool bb_ota_pause(void)
{
    bool paused = false;
    for (size_t i = 0; i < s_pair_count; i++) {
        if (s_pairs[i].pause && s_pairs[i].pause()) paused = true;
    }
    return paused;
}

bool bb_ota_has_pause_hook(void)
{
    for (size_t i = 0; i < s_pair_count; i++) {
        if (s_pairs[i].pause) return true;
    }
    return false;
}

// Resume iterates the same set (registration order) that pause used, so a
// 2nd registrant's resume always runs after its own pause fired.
void bb_ota_resume(void)
{
    for (size_t i = 0; i < s_pair_count; i++) {
        if (s_pairs[i].resume) s_pairs[i].resume();
    }
}

// Combine semantics: OR — skip if ANY registered skip-check hook returns true.
// Every registered hook is invoked exactly once (mirrors bb_ota_pause) so a
// later registrant's side effects are never dropped by an earlier true result.
bool bb_ota_skip_check(void)
{
    bool skip = false;
    for (size_t i = 0; i < s_skip_count; i++) {
        if (s_skip_cbs[i]) skip = s_skip_cbs[i]() || skip;
    }
    return skip;
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
    for (size_t i = 0; i < s_progress_count; i++) {
        if (s_progress_cbs[i]) s_progress_cbs[i](phase, pct);
    }

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
// Testing hooks
// ---------------------------------------------------------------------------

#ifdef BB_OTA_HOOKS_TESTING
void bb_ota_hooks_test_reset(void)
{
    s_pair_count     = 0;
    s_progress_count = 0;
    s_skip_count     = 0;
    s_last_phase     = BB_OTA_PHASE_FAIL;
    s_last_pct       = 0;
}
#endif

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
 * attach succeeds — mirrors bb_ota_check. */
BB_INIT_REGISTER_N(bb_ota_hooks, bb_ota_hooks_init, 4);
#endif

#endif // ESP_PLATFORM
