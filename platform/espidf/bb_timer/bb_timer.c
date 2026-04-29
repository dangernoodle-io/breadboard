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
