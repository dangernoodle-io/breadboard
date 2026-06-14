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

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <sys/time.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb_update_check";

// Jitter range in seconds applied to each periodic reschedule.
#define BB_UPDATE_CHECK_JITTER_S  600   // ±10 minutes
#define BB_UPDATE_CHECK_FLOOR_S    60   // minimum interval

static esp_timer_handle_t s_timer = NULL;

// Default: Core 1. On dual-core boards Core 0 carries lwip + wifi + httpd +
// the consumer's stratum/control-plane tasks; the mbedTLS handshake runs
// CPU-bound for 200–600 ms per attempt and would starve IDLE0 past the task
// watchdog if it landed there. Mirrors bb_ota_pull's default (Core 1).
// Consumers can opt out via bb_update_check_set_task_core(tskNO_AFFINITY)
// or pin elsewhere. See bb_update_check.h.
static int  s_task_core     = 1;
// Default worker priority is 1 (low). Consumers running the worker on a core
// that also hosts a high-priority CPU-bound task (e.g. mining at prio 20)
// must raise this above that task's priority via bb_update_check_set_task_priority.
static int  s_task_priority = 1;

// Concurrency guard: set (false→true) before spawning; cleared at the end of
// the one-shot task (or on spawn failure).  atomic_compare_exchange_strong is
// the race-safe gate so two concurrent timer fires / kicks cannot both proceed.
static atomic_bool s_check_in_flight = false;

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

    int task_core = s_task_core;
    if (task_core != tskNO_AFFINITY && task_core >= configNUMBER_OF_CORES) {
        task_core = tskNO_AFFINITY;
    }

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

    return true;
}

static void timer_cb(void *arg)
{
    (void)arg;
    try_spawn();
    // Reschedule one-shot timer with jitter so a fleet that rebooted together
    // drifts apart over time.
    esp_timer_start_once(s_timer, next_interval_us());
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

    // Create a one-shot timer; timer_cb reschedules it with ±10 min jitter
    // each tick so a fleet that rebooted together drifts apart over time.
    // No persistent worker task is created here — the one-shot worker is
    // spawned dynamically by try_spawn() on each timer fire or kick.
    esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .arg      = NULL,
        .name     = "upd_check",
    };
    err = esp_timer_create(&timer_args, &s_timer);
    if (err != BB_OK) return err;
    esp_timer_start_once(s_timer, next_interval_us());

#if defined(CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH) && CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH
    {
        // retained=true: update.available is a state topic — new SSE clients should
        // always receive the last known value even before the first periodic check fires.
        bb_err_t attach_err = bb_event_routes_attach_ex(BB_UPDATE_CHECK_TOPIC, true);
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

#if CONFIG_BB_UPDATE_CHECK_AUTOREGISTER
static bb_err_t bb_update_check_reserve_routes(void)
{
    bb_http_reserve_routes(3);  // GET /api/update/status + GET /api/update/config + POST /api/update/config
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_update_check, bb_update_check_reserve_routes);
BB_REGISTRY_REGISTER_N(bb_update_check, bb_update_check_register_init, 4);
#endif
