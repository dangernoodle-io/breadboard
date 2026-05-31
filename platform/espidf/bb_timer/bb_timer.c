#include "bb_timer.h"
#include "bb_log.h"
#include "esp_timer.h"
#include <stdlib.h>

static const char *TAG = "bb_timer";

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

    bb_timer_impl_t *impl = (bb_timer_impl_t *)malloc(sizeof(bb_timer_impl_t));
    if (impl == NULL) {
        bb_log_e(TAG, "malloc failed");
        return BB_ERR_NO_SPACE;
    }

    impl->type = type;
    impl->period_us = period_us;
    impl->cb = cb;
    impl->arg = arg;

    esp_timer_create_args_t timer_args = {
        .callback = dispatcher,
        .arg = impl,
        .name = name,
    };

    bb_err_t err = esp_timer_create(&timer_args, &impl->et);
    if (err != BB_OK) {
        bb_log_e(TAG, "esp_timer_create failed: %d", err);
        free(impl);
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
    // Return BB_OK even if not running (swallow ESP_ERR_INVALID_STATE)
    if (err == ESP_ERR_INVALID_STATE) {
        return BB_OK;
    }
    return err;
}

bb_err_t bb_timer_delete(bb_timer_handle_t h)
{
    if (h == NULL) {
        bb_log_e(TAG, "invalid handle");
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)h;
    esp_timer_stop(impl->et);  // Ignore error
    bb_err_t err = esp_timer_delete(impl->et);
    free(impl);
    return err;
}

uint64_t bb_timer_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

// ---------------------------------------------------------------------------
// Periodic timer API — ESP-IDF backend
// ---------------------------------------------------------------------------

struct bb_periodic_timer {
    esp_timer_handle_t h;
    void (*cb)(void *arg);
    void *arg;
};

static void periodic_dispatcher(void *arg)
{
    struct bb_periodic_timer *t = (struct bb_periodic_timer *)arg;
    t->cb(t->arg);
}

bb_err_t bb_timer_periodic_create(void (*cb)(void *arg), void *arg,
                                  const char *name, bb_periodic_timer_t *out)
{
    if (cb == NULL || out == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    struct bb_periodic_timer *t =
        (struct bb_periodic_timer *)malloc(sizeof(*t));
    if (t == NULL) {
        return BB_ERR_NO_SPACE;
    }

    t->cb  = cb;
    t->arg = arg;

    esp_timer_create_args_t args = {
        .callback        = periodic_dispatcher,
        .arg             = t,
        .name            = name,
        .dispatch_method = ESP_TIMER_TASK,
    };

    bb_err_t err = esp_timer_create(&args, &t->h);
    if (err != BB_OK) {
        free(t);
        return err;
    }

    *out = t;
    return BB_OK;
}

bb_err_t bb_timer_periodic_start(bb_periodic_timer_t t, uint64_t period_us)
{
    if (t == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    // esp_timer_start_periodic returns ESP_ERR_INVALID_STATE when already
    // running; stop first so callers can safely restart with a new period.
    esp_timer_stop(t->h);  // Ignore error — may not be running
    return esp_timer_start_periodic(t->h, period_us);
}

bb_err_t bb_timer_periodic_stop(bb_periodic_timer_t t)
{
    if (t == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t err = esp_timer_stop(t->h);
    if (err == ESP_ERR_INVALID_STATE) {
        return BB_OK;  // Already stopped — not an error
    }
    return err;
}

bb_err_t bb_timer_periodic_delete(bb_periodic_timer_t t)
{
    if (t == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    esp_timer_stop(t->h);  // Ignore error
    bb_err_t err = esp_timer_delete(t->h);
    free(t);
    return err;
}
