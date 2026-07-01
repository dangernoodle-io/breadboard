#include "bb_timer.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_task_registry.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <stdatomic.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_TIMER_DISP_STACK
#define BB_TIMER_DISP_STACK CONFIG_BB_TIMER_DISP_STACK
#endif
#ifdef CONFIG_BB_TIMER_DISP_PRIORITY
#define BB_TIMER_DISP_PRIORITY CONFIG_BB_TIMER_DISP_PRIORITY
#endif
#ifdef CONFIG_BB_TIMER_DISP_CORE
#define BB_TIMER_DISP_CORE CONFIG_BB_TIMER_DISP_CORE
#endif
#ifdef CONFIG_BB_TIMER_DISP_QUEUE_DEPTH
#define BB_TIMER_DISP_QUEUE_DEPTH CONFIG_BB_TIMER_DISP_QUEUE_DEPTH
#endif
#ifdef CONFIG_BB_TIMER_DISP_WDT_FEED_MS
#define BB_TIMER_DISP_WDT_FEED_MS CONFIG_BB_TIMER_DISP_WDT_FEED_MS
#endif
#endif

#ifndef BB_TIMER_DISP_WDT_FEED_MS
#define BB_TIMER_DISP_WDT_FEED_MS 5000
#endif

#ifndef BB_TIMER_DISP_STACK
#define BB_TIMER_DISP_STACK 4096
#endif
#ifndef BB_TIMER_DISP_PRIORITY
#define BB_TIMER_DISP_PRIORITY 5
#endif
#ifndef BB_TIMER_DISP_CORE
#define BB_TIMER_DISP_CORE -1
#endif
// On single-core targets core 1 does not exist; pinning the dispatcher there
// will panic at xTaskCreatePinnedToCore time.  Guard: affinity > 0 is only
// valid on dual-core builds.
#if CONFIG_FREERTOS_UNICORE
_Static_assert(BB_TIMER_DISP_CORE <= 0,
    "BB_TIMER_DISP_CORE must be <= 0 on FREERTOS_UNICORE targets (core 1 does "
    "not exist); set CONFIG_BB_TIMER_DISP_CORE to -1 (no affinity) or 0");
#endif
#ifndef BB_TIMER_DISP_QUEUE_DEPTH
#define BB_TIMER_DISP_QUEUE_DEPTH 16
#endif

static const char *TAG = "bb_timer";

/* Internal mode constants */
#define BB_TIMER_MODE_DIRECT  0
#define BB_TIMER_MODE_SHARED  1
#define BB_TIMER_MODE_WORKER  2

/* Shared dispatcher singleton */
typedef struct {
    void (*work_fn)(void *arg);
    void *arg;
} bb_disp_msg_t;

static QueueHandle_t s_disp_queue = NULL;
static TaskHandle_t  s_disp_task  = NULL;
// bb_timer_disp's own registry token, obtained from the register() call in
// disp_ensure_started (parent context). disp_task_fn may start running on a
// different core (xTaskCreatePinnedToCore) before that call returns and
// publishes the token, so the two halves are carried as independent atomics
// with a release/acquire pairing on the generation half — mirrors the
// release/acquire pairing bb_task_registry.c already uses internally for its
// per-entry generation counter. Publish: store index (relaxed) then
// generation (release). Consume: load generation (acquire) then index
// (relaxed) — release/acquire on `generation` establishes happens-before, so
// the consumer's index read is guaranteed to observe the publisher's index
// store. A pre-publish read yields BB_TASK_REGISTRY_TOKEN_INVALID (the init
// value below), which bb_task_registry_feed() treats as a harmless no-op —
// so no ordering dependency is required for correctness, only for promptly
// picking up the real token.
static _Atomic uint16_t s_disp_token_index = UINT16_MAX; /* BB_TASK_REGISTRY_TOKEN_INVALID.index */
static _Atomic uint16_t s_disp_token_gen   = 0;          /* BB_TASK_REGISTRY_TOKEN_INVALID.generation */

static void disp_task_fn(void *unused)
{
    (void)unused;
    bb_disp_msg_t msg;
#if defined(ESP_PLATFORM) && defined(CONFIG_BB_TIMER_DISP_WDT_ENABLE)
    for (;;) {
        /* Acquire consume — see s_disp_token_index/_gen comment above. */
        bb_task_registry_token_t token;
        token.generation = atomic_load_explicit(&s_disp_token_gen, memory_order_acquire);
        token.index      = atomic_load_explicit(&s_disp_token_index, memory_order_relaxed);
        if (xQueueReceive(s_disp_queue, &msg,
                          pdMS_TO_TICKS(BB_TIMER_DISP_WDT_FEED_MS)) == pdTRUE) {
            msg.work_fn(msg.arg);
            bb_task_registry_feed(token);
        } else {
            /* queue empty / idle timeout — feed to prove we are alive */
            bb_task_registry_feed(token);
        }
    }
#else
    for (;;) {
        if (xQueueReceive(s_disp_queue, &msg, portMAX_DELAY) == pdTRUE) {
            msg.work_fn(msg.arg);
        }
    }
#endif
}

static bb_err_t disp_ensure_started(void)
{
    if (s_disp_queue != NULL) return BB_OK;

    s_disp_queue = xQueueCreate(BB_TIMER_DISP_QUEUE_DEPTH, sizeof(bb_disp_msg_t));
    if (s_disp_queue == NULL) return BB_ERR_NO_SPACE;

    BaseType_t rc;
    if (BB_TIMER_DISP_CORE < 0) {
        rc = xTaskCreate(disp_task_fn, "bb_timer_disp", BB_TIMER_DISP_STACK,
                         NULL, BB_TIMER_DISP_PRIORITY, &s_disp_task);
    } else {
        rc = xTaskCreatePinnedToCore(disp_task_fn, "bb_timer_disp",
                                     BB_TIMER_DISP_STACK, NULL,
                                     BB_TIMER_DISP_PRIORITY, &s_disp_task,
                                     BB_TIMER_DISP_CORE);
    }
    if (rc != pdPASS) {
        vQueueDelete(s_disp_queue);
        s_disp_queue = NULL;
        return BB_ERR_NO_SPACE;
    }
    // Registry owns bb_timer_disp's hw WDT subscribe (parent context) —
    // same runtime effect as the task's former self-subscribe, gated by the
    // same Kconfig.
    bb_task_registry_opts_t opts = {
#if defined(ESP_PLATFORM) && defined(CONFIG_BB_TIMER_DISP_WDT_ENABLE)
        .hw_wdt_subscribe = true,
#else
        .hw_wdt_subscribe = false,
#endif
    };
    // Register into a local token, then publish via release stores — see the
    // s_disp_token_index/_gen comment above for why disp_task_fn (which may
    // already be running on the other core) must never see a torn/partial
    // write of the token here.
    bb_task_registry_token_t local_token = BB_TASK_REGISTRY_TOKEN_INVALID;
    bb_task_registry_register("bb_timer_disp", BB_TIMER_DISP_STACK, s_disp_task, &opts, &local_token);
    atomic_store_explicit(&s_disp_token_index, local_token.index, memory_order_relaxed);
    atomic_store_explicit(&s_disp_token_gen, local_token.generation, memory_order_release);
    return BB_OK;
}

/* Low-level generic timer */
typedef struct {
    esp_timer_handle_t et;
    bb_timer_type_t type;
    uint64_t period_us;
    bb_timer_cb_t cb;
    void *arg;
} bb_timer_impl_t;

static void dispatcher(void *arg)
{
    bb_timer_impl_t *impl = (bb_timer_impl_t *)arg;
    impl->cb(impl->arg);
}

bb_err_t bb_timer_create(const char *name, bb_timer_type_t type,
                         uint64_t period_us, bb_timer_cb_t cb, void *arg,
                         bb_timer_handle_t *out)
{
    if (out == NULL || cb == NULL) {
        bb_log_e(TAG, "invalid null argument");
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)bb_malloc_prefer_spiram(sizeof(bb_timer_impl_t));
    if (impl == NULL) {
        bb_log_e(TAG, "malloc failed");
        return BB_ERR_NO_SPACE;
    }

    impl->type      = type;
    impl->period_us = period_us;
    impl->cb        = cb;
    impl->arg       = arg;

    esp_timer_create_args_t timer_args = {
        .callback = dispatcher,
        .arg      = impl,
        .name     = name,
    };

    bb_err_t err = esp_timer_create(&timer_args, &impl->et);
    if (err != BB_OK) {
        bb_log_e(TAG, "esp_timer_create failed: %d", err);
        bb_mem_free(impl);
        return err;
    }

    *out = impl;
    return BB_OK;
}

bb_err_t bb_timer_start(bb_timer_handle_t h)
{
    if (h == NULL) {
        bb_log_e(TAG, "invalid handle");
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)h;
    if (impl->type == BB_TIMER_ONE_SHOT) {
        return esp_timer_start_once(impl->et, impl->period_us);
    } else {
        return esp_timer_start_periodic(impl->et, impl->period_us);
    }
}

bb_err_t bb_timer_stop(bb_timer_handle_t h)
{
    if (h == NULL) {
        bb_log_e(TAG, "invalid handle");
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)h;
    bb_err_t err = esp_timer_stop(impl->et);
    if (err == ESP_ERR_INVALID_STATE) return BB_OK;
    return err;
}

bb_err_t bb_timer_delete(bb_timer_handle_t h)
{
    if (h == NULL) {
        bb_log_e(TAG, "invalid handle");
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)h;
    esp_timer_stop(impl->et);
    bb_err_t err = esp_timer_delete(impl->et);
    bb_mem_free(impl);
    return err;
}

uint64_t IRAM_ATTR bb_timer_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

/* -------------------------------------------------------------------------
 * Periodic timer API — ESP-IDF backend
 * -------------------------------------------------------------------------*/

struct bb_periodic_timer {
    esp_timer_handle_t h;
    void (*cb)(void *arg);
    void *arg;
    int   mode;
    void (*work_fn)(void *arg);
    const char *name;
    volatile bool pending;
    /* MODE B */
    TaskHandle_t      worker_task;
    SemaphoreHandle_t worker_sem;
    volatile bool     worker_running;
};

struct bb_oneshot_timer {
    esp_timer_handle_t h;
    void (*cb)(void *arg);
    void *arg;
    int   mode;
    void (*work_fn)(void *arg);
    const char *name;
    volatile bool pending;
};

static void periodic_dispatcher(void *arg)
{
    struct bb_periodic_timer *t = (struct bb_periodic_timer *)arg;
    if (t->mode == BB_TIMER_MODE_DIRECT) {
        t->cb(t->arg);
    } else if (t->mode == BB_TIMER_MODE_SHARED) {
        bool expected = false;
        if (!atomic_compare_exchange_strong(
                (atomic_bool *)&t->pending, &expected, true)) {
            return;
        }
        bb_disp_msg_t msg = { .work_fn = t->work_fn, .arg = t->arg };
        if (xQueueSend(s_disp_queue, &msg, 0) != pdTRUE) {
            t->pending = false;
        }
        t->pending = false;
    } else if (t->mode == BB_TIMER_MODE_WORKER) {
        if (t->worker_sem) xSemaphoreGive(t->worker_sem);
    }
}

static void worker_task_fn(void *arg)
{
    struct bb_periodic_timer *t = (struct bb_periodic_timer *)arg;
    while (t->worker_running) {
        if (xSemaphoreTake(t->worker_sem, portMAX_DELAY) == pdTRUE) {
            if (!t->worker_running) break;
            t->work_fn(t->arg);
        }
    }
    bb_task_registry_deregister(xTaskGetCurrentTaskHandle());
    vTaskDelete(NULL);
}

static void oneshot_dispatcher(void *arg)
{
    struct bb_oneshot_timer *t = (struct bb_oneshot_timer *)arg;
    if (t->mode == BB_TIMER_MODE_DIRECT) {
        t->cb(t->arg);
    } else if (t->mode == BB_TIMER_MODE_SHARED) {
        bool expected = false;
        if (!atomic_compare_exchange_strong(
                (atomic_bool *)&t->pending, &expected, true)) {
            return;
        }
        bb_disp_msg_t msg = { .work_fn = t->work_fn, .arg = t->arg };
        if (xQueueSend(s_disp_queue, &msg, 0) != pdTRUE) {
            t->pending = false;
        }
        t->pending = false;
    }
}

bb_err_t bb_timer_periodic_create(void (*cb)(void *arg), void *arg,
                                  const char *name, bb_periodic_timer_t *out)
{
    if (cb == NULL || out == NULL) return BB_ERR_INVALID_ARG;

    struct bb_periodic_timer *t =
        (struct bb_periodic_timer *)bb_malloc_prefer_spiram(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;

    t->cb             = cb;
    t->arg            = arg;
    t->mode           = BB_TIMER_MODE_DIRECT;
    t->work_fn        = NULL;
    t->name           = name;
    t->pending        = false;
    t->worker_task    = NULL;
    t->worker_sem     = NULL;
    t->worker_running = false;

    esp_timer_create_args_t args = {
        .callback        = periodic_dispatcher,
        .arg             = t,
        .name            = name,
        .dispatch_method = ESP_TIMER_TASK,
    };

    bb_err_t err = esp_timer_create(&args, &t->h);
    if (err != BB_OK) {
        bb_mem_free(t);
        return err;
    }

    *out = t;
    return BB_OK;
}

bb_err_t bb_timer_periodic_start(bb_periodic_timer_t t, uint64_t period_us)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    esp_timer_stop(t->h);
    return esp_timer_start_periodic(t->h, period_us);
}

bb_err_t bb_timer_periodic_stop(bb_periodic_timer_t t)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    bb_err_t err = esp_timer_stop(t->h);
    if (err == ESP_ERR_INVALID_STATE) return BB_OK;
    return err;
}

bb_err_t bb_timer_periodic_delete(bb_periodic_timer_t t)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    esp_timer_stop(t->h);
    bb_err_t err = esp_timer_delete(t->h);
    if (t->mode == BB_TIMER_MODE_WORKER && t->worker_sem) {
        t->worker_running = false;
        xSemaphoreGive(t->worker_sem);
        vTaskDelay(pdMS_TO_TICKS(10));
        vSemaphoreDelete(t->worker_sem);
    }
    bb_mem_free(t);
    return err;
}

bb_err_t bb_timer_oneshot_create(void (*cb)(void *arg), void *arg,
                                 const char *name, bb_oneshot_timer_t *out)
{
    if (cb == NULL || out == NULL) return BB_ERR_INVALID_ARG;

    struct bb_oneshot_timer *t =
        (struct bb_oneshot_timer *)bb_malloc_prefer_spiram(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;

    t->cb      = cb;
    t->arg     = arg;
    t->mode    = BB_TIMER_MODE_DIRECT;
    t->work_fn = NULL;
    t->name    = name;
    t->pending = false;

    esp_timer_create_args_t args = {
        .callback        = oneshot_dispatcher,
        .arg             = t,
        .name            = name,
        .dispatch_method = ESP_TIMER_TASK,
    };

    bb_err_t err = esp_timer_create(&args, &t->h);
    if (err != BB_OK) {
        bb_mem_free(t);
        return err;
    }

    *out = t;
    return BB_OK;
}

bb_err_t bb_timer_oneshot_start(bb_oneshot_timer_t t, uint64_t delay_us)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    esp_timer_stop(t->h);
    return esp_timer_start_once(t->h, delay_us);
}

bb_err_t bb_timer_oneshot_stop(bb_oneshot_timer_t t)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    bb_err_t err = esp_timer_stop(t->h);
    if (err == ESP_ERR_INVALID_STATE) return BB_OK;
    return err;
}

bb_err_t bb_timer_oneshot_delete(bb_oneshot_timer_t t)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    esp_timer_stop(t->h);
    bb_err_t err = esp_timer_delete(t->h);
    bb_mem_free(t);
    return err;
}

/* -------------------------------------------------------------------------
 * MODE A: Deferred timer API — ESP-IDF backend
 * -------------------------------------------------------------------------*/

bb_err_t bb_timer_deferred_periodic_create(void (*work_fn)(void *arg), void *arg,
                                           const char *name, bb_periodic_timer_t *out)
{
    if (work_fn == NULL || out == NULL) return BB_ERR_INVALID_ARG;

    bb_err_t err = disp_ensure_started();
    if (err != BB_OK) return err;

    struct bb_periodic_timer *t =
        (struct bb_periodic_timer *)bb_malloc_prefer_spiram(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;

    t->cb             = NULL;
    t->arg            = arg;
    t->mode           = BB_TIMER_MODE_SHARED;
    t->work_fn        = work_fn;
    t->name           = name;
    t->pending        = false;
    t->worker_task    = NULL;
    t->worker_sem     = NULL;
    t->worker_running = false;

    esp_timer_create_args_t args = {
        .callback        = periodic_dispatcher,
        .arg             = t,
        .name            = name,
        .dispatch_method = ESP_TIMER_TASK,
    };

    err = esp_timer_create(&args, &t->h);
    if (err != BB_OK) {
        bb_mem_free(t);
        return err;
    }

    *out = t;
    return BB_OK;
}

bb_err_t bb_timer_deferred_oneshot_create(void (*work_fn)(void *arg), void *arg,
                                          const char *name, bb_oneshot_timer_t *out)
{
    if (work_fn == NULL || out == NULL) return BB_ERR_INVALID_ARG;

    bb_err_t err = disp_ensure_started();
    if (err != BB_OK) return err;

    struct bb_oneshot_timer *t =
        (struct bb_oneshot_timer *)bb_malloc_prefer_spiram(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;

    t->cb      = NULL;
    t->arg     = arg;
    t->mode    = BB_TIMER_MODE_SHARED;
    t->work_fn = work_fn;
    t->name    = name;
    t->pending = false;

    esp_timer_create_args_t args = {
        .callback        = oneshot_dispatcher,
        .arg             = t,
        .name            = name,
        .dispatch_method = ESP_TIMER_TASK,
    };

    err = esp_timer_create(&args, &t->h);
    if (err != BB_OK) {
        bb_mem_free(t);
        return err;
    }

    *out = t;
    return BB_OK;
}

/* -------------------------------------------------------------------------
 * MODE B: Worker timer API — ESP-IDF backend
 * -------------------------------------------------------------------------*/

bb_err_t bb_timer_worker_periodic_create(void (*work_fn)(void *arg), void *arg,
                                         const char *name,
                                         const bb_timer_worker_cfg_t *cfg,
                                         bb_periodic_timer_t *out)
{
    if (work_fn == NULL || out == NULL) return BB_ERR_INVALID_ARG;

    uint32_t stack    = (cfg && cfg->stack)    ? cfg->stack    : 4096;
    int      priority = (cfg && cfg->priority) ? cfg->priority : BB_TIMER_DISP_PRIORITY;
    int      core     = cfg                    ? cfg->core     : BB_TIMER_DISP_CORE;

    struct bb_periodic_timer *t =
        (struct bb_periodic_timer *)bb_malloc_prefer_spiram(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;

    t->cb             = NULL;
    t->arg            = arg;
    t->mode           = BB_TIMER_MODE_WORKER;
    t->work_fn        = work_fn;
    t->name           = name;
    t->pending        = false;
    t->worker_task    = NULL;
    t->worker_running = true;

    t->worker_sem = xSemaphoreCreateBinary();
    if (t->worker_sem == NULL) {
        bb_mem_free(t);
        return BB_ERR_NO_SPACE;
    }

    BaseType_t rc;
    if (core < 0) {
        rc = xTaskCreate(worker_task_fn, name ? name : "bb_timer_worker",
                         stack, t, priority, &t->worker_task);
    } else {
        rc = xTaskCreatePinnedToCore(worker_task_fn,
                                     name ? name : "bb_timer_worker",
                                     stack, t, priority, &t->worker_task, core);
    }
    if (rc != pdPASS) {
        vSemaphoreDelete(t->worker_sem);
        bb_mem_free(t);
        return BB_ERR_NO_SPACE;
    }
    bb_task_registry_register(name ? name : "bb_timer_worker", stack, t->worker_task, NULL, NULL);

    esp_timer_create_args_t args = {
        .callback        = periodic_dispatcher,
        .arg             = t,
        .name            = name,
        .dispatch_method = ESP_TIMER_TASK,
    };

    bb_err_t err = esp_timer_create(&args, &t->h);
    if (err != BB_OK) {
        t->worker_running = false;
        xSemaphoreGive(t->worker_sem);
        vTaskDelay(pdMS_TO_TICKS(10));
        vSemaphoreDelete(t->worker_sem);
        bb_mem_free(t);
        return err;
    }

    *out = t;
    return BB_OK;
}
