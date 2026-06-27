// ESP-IDF route handler + on-demand worker spawn for bb_update_check.
//
// Worker model (B1-240): the periodic timer and bb_update_check_kick() spawn a
// short-lived task that runs bb_update_check_run_one() once then exits.  The
// 8 KB stack is dynamically allocated only for the duration of a fetch; it is
// freed as soon as the task calls vTaskDelete(NULL).  Between checks (the
// overwhelming majority of the device lifetime) the stack does not exist.
//
// Concurrency guard: s_check_in_flight is an atomic_bool.  Before spawning
// the task the caller does an atomic compare-exchange from false→true.  If the
// slot is already taken the new spawn is skipped silently.  The one-shot task
// clears the flag before deleting itself so the next timer tick can spawn
// again.  If xTaskCreate fails (heap too fragmented for the 8 KB stack), the
// flag is cleared immediately and a warning is logged; the next timer tick
// retries.
#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_http.h"
#include "bb_http_client.h"
#include "bb_log.h"
#include "bb_registry.h"
#include "bb_event_routes.h"
#include "bb_claim.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <sys/time.h>

#include "bb_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb_update_check";

// Jitter and floor tunables — override via CONFIG_BB_UPDATE_CHECK_JITTER_S and
// CONFIG_BB_UPDATE_CHECK_FLOOR_S (Kconfig). Defaults match prior hardcoded values.
#ifndef CONFIG_BB_UPDATE_CHECK_JITTER_S
#define CONFIG_BB_UPDATE_CHECK_JITTER_S 600
#endif
#ifndef CONFIG_BB_UPDATE_CHECK_FLOOR_S
#define CONFIG_BB_UPDATE_CHECK_FLOOR_S 60
#endif
#define BB_UPDATE_CHECK_JITTER_S CONFIG_BB_UPDATE_CHECK_JITTER_S
#define BB_UPDATE_CHECK_FLOOR_S  CONFIG_BB_UPDATE_CHECK_FLOOR_S

static bb_oneshot_timer_t s_timer = NULL;

// Default core/priority from Kconfig — both fallback to 1 for host builds
// that don't include Kconfig. Runtime setters override these values.
#ifndef CONFIG_BB_UPDATE_CHECK_TASK_CORE
#define CONFIG_BB_UPDATE_CHECK_TASK_CORE 1
#endif
#ifndef CONFIG_BB_UPDATE_CHECK_TASK_PRIORITY
#define CONFIG_BB_UPDATE_CHECK_TASK_PRIORITY 1
#endif

// Default: Core 1. On dual-core boards Core 0 carries lwip + wifi + httpd +
// the consumer's stratum/control-plane tasks; the mbedTLS handshake runs
// CPU-bound for 200–600 ms per attempt and would starve IDLE0 past the task
// watchdog if it landed there. Mirrors bb_ota_pull's default (Core 1).
// Consumers can opt out via bb_update_check_set_task_core(tskNO_AFFINITY)
// or pin elsewhere. See bb_update_check.h.
static int  s_task_core     = CONFIG_BB_UPDATE_CHECK_TASK_CORE;
// Default worker priority. Consumers running the worker on a core that also
// hosts a high-priority CPU-bound task (e.g. mining at prio 20) must raise
// this above that task's priority via bb_update_check_set_task_priority.
static int  s_task_priority = CONFIG_BB_UPDATE_CHECK_TASK_PRIORITY;

// Concurrency guard: set (false→true) before spawning; cleared at the end of
// the one-shot task (or on spawn failure).  atomic_compare_exchange_strong is
// the race-safe gate so two concurrent timer fires / kicks cannot both proceed.
static atomic_bool s_check_in_flight = false;

#if CONFIG_BB_OTA_STATIC_STACK && CONFIG_BB_UPDATE_CHECK_AUTOREGISTER
static StaticTask_t s_upd_check_task_buf;
static StackType_t  s_upd_check_stack[BB_HTTP_CLIENT_TASK_STACK / sizeof(StackType_t)];
#endif

// OTA operation exclusive-slot claim. ota_pull acquires "ota_pull" before
// spawning the download worker; upd_check acquires "upd_check" before the
// fetch. Only one OTA-class operation runs at a time.
static bb_claim_t s_ota_claim = BB_CLAIM_INIT;

// Compute next poll interval: CONFIG_BB_UPDATE_CHECK_INTERVAL_S ± jitter,
// floored at BB_UPDATE_CHECK_FLOOR_S.  Uses esp_random() for uniform jitter.
static uint64_t next_interval_us(void)
{
    uint32_t r = esp_random();
    int32_t  offset_s = (int32_t)(r % (uint32_t)(2 * BB_UPDATE_CHECK_JITTER_S + 1))
                        - BB_UPDATE_CHECK_JITTER_S;
    int32_t  interval_s = (int32_t)CONFIG_BB_UPDATE_CHECK_INTERVAL_S + offset_s;
    if (interval_s < BB_UPDATE_CHECK_FLOOR_S) {
        interval_s = BB_UPDATE_CHECK_FLOOR_S;
    }
    return (uint64_t)interval_s * 1000000ULL;
}

// One-shot task body: run the manifest fetch then exit.
// The stack exists only for the lifetime of this function.
static void ondemand_task(void *arg)
{
    (void)arg;
    bb_update_check_run_one();
    bb_claim_release(&s_ota_claim, "upd_check");
    // Clear the in-flight guard before deleting so the next kick can spawn.
    atomic_store(&s_check_in_flight, false);
    vTaskDelete(NULL);
}

// Try to spawn the one-shot worker.  Returns true if spawned (or already in
// flight), false if xTaskCreate failed.
static bool try_spawn(void)
{
    // Atomically claim the in-flight slot.
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_check_in_flight, &expected, true)) {
        // Already in flight — skip silently.
        bb_log_d(TAG, "check already in flight, skipping spawn");
        return true;
    }

    // Claim the OTA exclusive slot. If ota_pull is active, yield this spawn.
    if (bb_claim_acquire(&s_ota_claim, "upd_check") != BB_OK) {
        atomic_store(&s_check_in_flight, false);
        bb_log_w(TAG, "upd_check: ota_pull holds OTA slot, skipping this interval");
        return false;
    }

    int task_core = s_task_core;
    if (task_core != tskNO_AFFINITY && task_core >= configNUMBER_OF_CORES) {
        task_core = tskNO_AFFINITY;
    }

#if CONFIG_BB_OTA_STATIC_STACK && CONFIG_BB_UPDATE_CHECK_AUTOREGISTER
    TaskHandle_t upd_task = xTaskCreateStaticPinnedToCore(
        ondemand_task, "upd_check", BB_HTTP_CLIENT_TASK_STACK / sizeof(StackType_t),
        NULL, s_task_priority, s_upd_check_stack, &s_upd_check_task_buf, task_core);
    if (!upd_task) {
        atomic_store(&s_check_in_flight, false);
        bb_log_w(TAG, "spawn failed; will retry next interval");
        return false;
    }
#else
    BaseType_t rc = xTaskCreatePinnedToCore(
        ondemand_task, "upd_check", BB_HTTP_CLIENT_TASK_STACK,
        NULL, s_task_priority, NULL, task_core);

    if (rc != pdPASS) {
        // Heap too fragmented / low for the 8 KB stack at this moment.
        // Clear the guard and log a warning; the next timer tick will retry.
        atomic_store(&s_check_in_flight, false);
        bb_log_w(TAG, "spawn failed (low heap?); will retry next interval");
        return false;
    }
#endif

    return true;
}

static void timer_work_fn(void *arg)
{
    (void)arg;
    try_spawn();
    // Reschedule one-shot timer with jitter so a fleet that rebooted together
    // drifts apart over time.
    bb_timer_oneshot_start(s_timer, next_interval_us());
}

// ---------------------------------------------------------------------------
// Non-blocking kick (public API)
// ---------------------------------------------------------------------------

bb_err_t bb_update_check_kick(void)
{
    if (!s_timer) return BB_ERR_INVALID_STATE;
    try_spawn();
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Kick + wait for completion (public API)
// ---------------------------------------------------------------------------

// Poll interval and total timeout for bb_update_check_run_blocking.
#define BB_UPDATE_CHECK_BLOCKING_POLL_MS  100

bb_err_t bb_update_check_run_blocking(uint32_t timeout_ms)
{
    if (!s_timer) return BB_ERR_INVALID_STATE;

    // Sample last_check_us before the kick so we can detect the worker
    // completing by observing it advance (the one-shot task always writes a
    // non-zero last_check_us on both success and failure paths).
    bb_update_check_status_t before;
    bb_err_t err = bb_update_check_get_status(&before);
    if (err != BB_OK) return BB_ERR_INVALID_STATE;
    int64_t pre_check_us = before.last_check_us;

    try_spawn();

    uint32_t elapsed_ms = 0;
    while (elapsed_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(BB_UPDATE_CHECK_BLOCKING_POLL_MS));
        elapsed_ms += BB_UPDATE_CHECK_BLOCKING_POLL_MS;

        bb_update_check_status_t after;
        if (bb_update_check_get_status(&after) != BB_OK) break;
        if (after.last_check_us != pre_check_us) {
            return BB_OK;
        }
    }

    bb_log_w(TAG, "run_blocking: timed out after %" PRIu32 " ms", elapsed_ms);
    return BB_ERR_TIMEOUT;
}

// ---------------------------------------------------------------------------
// GET /api/update/status
// ---------------------------------------------------------------------------

static bb_err_t status_handler(bb_http_request_t *req)
{
    return bb_update_check_emit_status_json(req);
}

static const bb_route_response_t s_status_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
       "\"properties\":{"
         "\"current\":{\"type\":\"string\"},"
         "\"latest\":{\"type\":\"string\"},"
         "\"download_url\":{\"type\":\"string\"},"
         "\"available\":{\"type\":\"boolean\"},"
         "\"last_check_ok\":{\"type\":\"boolean\"},"
         "\"enabled\":{\"type\":\"boolean\"},"
         "\"outcome\":{\"type\":\"string\","
           "\"enum\":[\"unknown\",\"up_to_date\",\"available\","
                     "\"no_asset\",\"check_failed\"]},"
         "\"last_check_ts\":{\"type\":\"integer\"}},"
       "\"required\":[\"current\",\"latest\",\"download_url\","
                     "\"available\",\"last_check_ok\",\"enabled\",\"outcome\"]}",
      "current status of the update poller" },
    { 503, "application/json", NULL, "bb_update_check not initialized" },
    { 0 },
};

static const bb_route_t s_status_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/update/status",
    .tag      = "update",
    .summary  = "Latest known release-check state",
    .responses = s_status_responses,
    .handler  = status_handler,
};

static bb_err_t bb_update_check_register_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_update_check_init(NULL);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_status_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, bb_update_check_config_get_route());
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, bb_update_check_config_post_route());
    if (err != BB_OK) return err;

    // Create a one-shot timer; timer_work_fn reschedules it with ±10 min
    // jitter each tick so a fleet that rebooted together drifts apart over
    // time.  No persistent worker task is created here — the one-shot worker
    // is spawned dynamically by try_spawn() on each timer fire or kick.
    err = bb_timer_deferred_oneshot_create(timer_work_fn, NULL, "upd_check",
                                           &s_timer);
    if (err != BB_OK) return err;
    bb_timer_oneshot_start(s_timer, next_interval_us());

#if defined(CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH) && CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH
    {
        // retained=true: update.available is a state topic — new SSE clients should
        // always receive the last known value even before the first periodic check fires.
        // max_entry=512: union payload worst-cases ~430 B; global default (256) is too
        // small, but only this topic needs the bump (+256 B, not the global ~4 KB cost).
        bb_err_t attach_err = bb_event_routes_attach_ex2(BB_UPDATE_CHECK_TOPIC, true, 512);
        if (attach_err != BB_OK) {
            bb_log_w(TAG, "auto-attach failed for 'update.available': %d", attach_err);
        }

        // Now that the ring is attached, publish the initial snapshot so that SSE
        // clients connecting before the first periodic check (up to
        // CONFIG_BB_UPDATE_CHECK_INTERVAL_S seconds) replay this entry rather than
        // seeing empty state.
        err = bb_update_check_publish_initial();
        if (err != BB_OK) {
            bb_log_w(TAG, "failed to publish initial snapshot: %d", err);
        }
    }
#endif

    bb_log_i(TAG, "registered /api/update/status; period=%" PRIu32 " s (on-demand worker)",
             (uint32_t)CONFIG_BB_UPDATE_CHECK_INTERVAL_S);
    return BB_OK;
}

void bb_update_check_set_task_core(int core)
{
    s_task_core = core;
}

void bb_update_check_set_task_priority(int priority)
{
    s_task_priority = priority;
}

#ifdef BB_UPDATE_CHECK_TESTING
// ESP-IDF test hooks for the in-flight guard (used by on-device test builds).
void bb_update_check_set_in_flight_for_test(bool in_flight)
{
    atomic_store(&s_check_in_flight, in_flight);
}

bool bb_update_check_get_in_flight_for_test(void)
{
    return atomic_load(&s_check_in_flight);
}
#endif

// ---------------------------------------------------------------------------
// Public OTA-claim accessors (used by bb_ota_pull)
// ---------------------------------------------------------------------------

bb_err_t bb_update_check_ota_claim_acquire(const char *id)
{
    return bb_claim_acquire(&s_ota_claim, id);
}

void bb_update_check_ota_claim_release(const char *id)
{
    bb_claim_release(&s_ota_claim, id);
}

#ifdef BB_UPDATE_CHECK_TESTING
void bb_update_check_ota_claim_reset(void)
{
    bb_claim_reset(&s_ota_claim);
}
#endif

#if CONFIG_BB_UPDATE_CHECK_AUTOREGISTER
static bb_err_t bb_update_check_reserve_routes(void)
{
    bb_http_reserve_routes(3);  // GET /api/update/status + GET /api/update/config + POST /api/update/config
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_update_check, bb_update_check_reserve_routes);
BB_REGISTRY_REGISTER_N(bb_update_check, bb_update_check_register_init, 4);
#endif
