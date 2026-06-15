// bb_pub ESP-IDF timer + worker task implementation.
//
// The periodic timer fires every CONFIG_BB_PUB_INTERVAL_MS and gives a
// semaphore to wake the worker. The worker calls bb_pub_tick_once(). This
// keeps all JSON allocation and publish IO off the timer ISR context.
//
// Stack budget: see CONFIG_BB_PUB_WORKER_STACK (Kconfig, default 8192).
// The worker calls each sink's publish() synchronously. An HTTP/TLS sink
// (bb_sink_http over HTTPS via bb_http_client_post) needs >= 8192 bytes for
// the mbedTLS handshake. MQTT-only or plaintext-HTTP sinks can drop to 4096
// to save RAM. The default is sized for the heaviest (TLS) case.
#include "bb_pub.h"
#include "bb_log.h"
#include "bb_timer.h"
#include "bb_registry.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <inttypes.h>

static const char *TAG = "bb_pub";

// Worker task stack in bytes. Tunable via CONFIG_BB_PUB_WORKER_STACK (Kconfig).
// Default 8192: covers mbedTLS handshake for HTTP/TLS sinks (bb_sink_http).
#ifndef BB_PUB_WORKER_STACK
#  if defined(CONFIG_BB_PUB_WORKER_STACK)
#    define BB_PUB_WORKER_STACK CONFIG_BB_PUB_WORKER_STACK
#  else
#    define BB_PUB_WORKER_STACK 8192
#  endif
#endif

// Worker task priority. Tunable via CONFIG_BB_PUB_WORKER_PRIORITY (Kconfig).
// Default 1 (lowest app priority). Raise when competing with CPU-bound tasks.
#ifndef CONFIG_BB_PUB_WORKER_PRIORITY
#define CONFIG_BB_PUB_WORKER_PRIORITY 1
#endif

static bb_periodic_timer_t s_timer  = NULL;
static SemaphoreHandle_t   s_kick   = NULL;
static TaskHandle_t        s_worker = NULL;

static void worker_task(void *arg)
{
    (void)arg;
    while (1) {
        xSemaphoreTake(s_kick, portMAX_DELAY);
        bb_pub_tick_once();
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    if (s_kick) xSemaphoreGive(s_kick);
}

// Hook called by bb_pub_set_interval_ms when a new interval is persisted.
// Re-arms the periodic timer with the new period without reboot.
static void interval_apply_hook(uint32_t ms)
{
    if (!s_timer) return;
    bb_err_t err = bb_timer_periodic_start(s_timer, (uint64_t)ms * 1000ULL);
    if (err != BB_OK) {
        bb_log_w(TAG, "interval_apply_hook: timer re-arm failed: %d", err);
    } else {
        bb_log_i(TAG, "publish interval updated to %"PRIu32" ms", ms);
    }
}

static bb_err_t bb_pub_start(void)
{
    s_kick = xSemaphoreCreateBinary();
    if (!s_kick) return BB_ERR_NO_SPACE;

    if (xTaskCreate(worker_task, "bb_pub", BB_PUB_WORKER_STACK,
                    NULL, CONFIG_BB_PUB_WORKER_PRIORITY, &s_worker) != pdPASS) {
        vSemaphoreDelete(s_kick);
        s_kick = NULL;
        return BB_ERR_INVALID_STATE;
    }

    bb_err_t err = bb_timer_periodic_create(timer_cb, NULL, "bb_pub", &s_timer);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to create periodic timer: %d", err);
        return err;
    }

    // Register hook before loading interval so set_interval_ms can re-arm.
    bb_pub_set_interval_apply_hook(interval_apply_hook);

    // Use the NVS-persisted interval (or compile-time default) for initial start.
    uint32_t interval_ms = bb_pub_get_interval_ms();
    err = bb_timer_periodic_start(s_timer, (uint64_t)interval_ms * 1000ULL);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to start periodic timer: %d", err);
        return err;
    }

    bb_log_i(TAG, "started; interval=%"PRIu32" ms", interval_ms);
    return BB_OK;
}

#if CONFIG_BB_PUB_AUTOREGISTER
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub, bb_pub_start);
#endif
