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

static const char *TAG = "bb_pub";

#ifndef CONFIG_BB_PUB_INTERVAL_MS
#define CONFIG_BB_PUB_INTERVAL_MS 10000
#endif

// Worker task stack in bytes. Tunable via CONFIG_BB_PUB_WORKER_STACK (Kconfig).
// Default 8192: covers mbedTLS handshake for HTTP/TLS sinks (bb_sink_http).
#ifndef BB_PUB_WORKER_STACK
#  if defined(CONFIG_BB_PUB_WORKER_STACK)
#    define BB_PUB_WORKER_STACK CONFIG_BB_PUB_WORKER_STACK
#  else
#    define BB_PUB_WORKER_STACK 8192
#  endif
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

static bb_err_t bb_pub_start(void)
{
    s_kick = xSemaphoreCreateBinary();
    if (!s_kick) return BB_ERR_NO_SPACE;

    if (xTaskCreate(worker_task, "bb_pub", BB_PUB_WORKER_STACK,
                    NULL, 1, &s_worker) != pdPASS) {
        vSemaphoreDelete(s_kick);
        s_kick = NULL;
        return BB_ERR_INVALID_STATE;
    }

    bb_err_t err = bb_timer_periodic_create(timer_cb, NULL, "bb_pub", &s_timer);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to create periodic timer: %d", err);
        return err;
    }

    err = bb_timer_periodic_start(s_timer,
                                  (uint64_t)CONFIG_BB_PUB_INTERVAL_MS * 1000ULL);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to start periodic timer: %d", err);
        return err;
    }

    bb_log_i(TAG, "started; interval=%d ms", CONFIG_BB_PUB_INTERVAL_MS);
    return BB_OK;
}

#if CONFIG_BB_PUB_AUTOREGISTER
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub, bb_pub_start);
#endif
