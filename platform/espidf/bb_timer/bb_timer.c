#include "bb_timer.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_task.h"
#include "bb_wdt.h"
#include "bb_once.h"
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

// s_disp_queue/s_disp_task bootstrap (disp_ensure_started(), below) used to
// be an unguarded check-create-assign race: two concurrent callers (dual-
// core) could each pass the s_disp_queue==NULL check and each create+assign
// a queue/task, leaking one pair and racing the assignment itself.
//
// This bootstrap CAN fail (queue/task create), so it cannot be a plain
// bb_once_run() body -- a failed run must stay retryable on the next call,
// not permanently latch. It ALSO must not guard a separate bb_lock_t via a
// plain bb_once_run() (an earlier revision of this fix did exactly that,
// and reproduced the same permanent-latch bug one level down: bb_lock_init()
// itself heap-allocates and can fail, so bb_once_run() would mark that
// bootstrap "done" even on a failed lock init, and every later
// disp_ensure_started() call would then dereference an uninitialized
// bb_lock_t forever). bb_once_run_fallible() (B1-524) is built for exactly
// this shape: the fallible queue/task creation runs directly inside its fn,
// gated on s_disp_queue == NULL as its own "not done yet" sentinel (defense
// in depth, re-checked by disp_ensure_started() below); a failed attempt
// resets the guard to IDLE so the very next caller genuinely retries
// instead of replaying a cached failure or crashing on a half-built lock.
static bb_once_t s_disp_once = BB_ONCE_INIT;

static void disp_task_fn(void *unused)
{
    (void)unused;
    bb_disp_msg_t msg;
#if defined(ESP_PLATFORM) && defined(CONFIG_BB_TIMER_DISP_WDT_ENABLE)
    // Self-subscribe at task entry (cycle-breaking migration onto
    // bb_task_create, task-registry unification PR3) — same runtime effect
    // as the former parent-context bb_task_registry_register(hw_wdt_subscribe)
    // call, and tighter: this is the very first statement the task runs, vs.
    // the old parent-side subscribe which raced the task's own first
    // scheduler slice on the other core.
    bb_wdt_task_subscribe();
    for (;;) {
        if (xQueueReceive(s_disp_queue, &msg,
                          pdMS_TO_TICKS(BB_TIMER_DISP_WDT_FEED_MS)) == pdTRUE) {
            msg.work_fn(msg.arg);
            bb_wdt_task_feed();
        } else {
            /* queue empty / idle timeout — feed to prove we are alive */
            bb_wdt_task_feed();
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

// bb_once_run_fallible() body: returns true only on a fully successful
// queue+task bootstrap. s_disp_queue == NULL both gates re-entry (a caller
// that raced in after DONE skips straight past, matching the old
// early-return) and marks a failed attempt for retry -- it is left NULL on
// every failure path below.
static bool disp_bootstrap(void *ctx)
{
    (void)ctx;
    if (s_disp_queue != NULL) return true;

    s_disp_queue = xQueueCreate(BB_TIMER_DISP_QUEUE_DEPTH, sizeof(bb_disp_msg_t));
    if (s_disp_queue == NULL) return false;

    bb_task_config_t cfg = {
        .entry       = disp_task_fn,
        .name        = "bb_timer_disp",
        .arg         = NULL,
        .stack_bytes = BB_TIMER_DISP_STACK,
        .priority    = BB_TIMER_DISP_PRIORITY,
        .core        = BB_TIMER_DISP_CORE,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        // Data flag only, surfaced at GET /api/diag/tasks — the actual hw
        // WDT subscribe is self-performed inside disp_task_fn above.
#if defined(ESP_PLATFORM) && defined(CONFIG_BB_TIMER_DISP_WDT_ENABLE)
        .wdt_arm     = true,
#else
        .wdt_arm     = false,
#endif
    };
    bb_err_t err = bb_task_create(&cfg, (void **)&s_disp_task);
    if (err != BB_OK) {
        vQueueDelete(s_disp_queue);
        s_disp_queue = NULL;
        return false;
    }
    return true;
}

// Retry-safe bootstrap (see s_disp_once's declaration comment above) --
// bb_once_run_fallible() serializes every caller through disp_bootstrap()
// so exactly one caller ever runs the create path at a time (real
// blocking/yielding wait, no busy-spin), and the s_disp_queue == NULL
// re-check below (not the fallible-once's own return value) is the
// defense-in-depth guard: a transient failure leaves it NULL so the next
// caller genuinely retries rather than replaying a cached failure.
static bb_err_t disp_ensure_started(void)
{
    bb_once_run_fallible(&s_disp_once, disp_bootstrap, NULL);
    return (s_disp_queue != NULL) ? BB_OK : BB_ERR_NO_SPACE;
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
    bb_task_deregister(xTaskGetCurrentTaskHandle());
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

    bb_task_config_t task_cfg = {
        .entry       = worker_task_fn,
        .name        = name ? name : "bb_timer_worker",
        .arg         = t,
        .stack_bytes = stack,
        .priority    = (uint32_t)priority,
        .core        = core,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    bb_err_t task_err = bb_task_create(&task_cfg, (void **)&t->worker_task);
    if (task_err != BB_OK) {
        vSemaphoreDelete(t->worker_sem);
        bb_mem_free(t);
        return BB_ERR_NO_SPACE;
    }

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
