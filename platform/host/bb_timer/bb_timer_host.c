#include "bb_timer.h"
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>

typedef struct {
    bb_timer_type_t type;
    uint64_t period_us;
    bb_timer_cb_t cb;
    void *arg;
    pthread_t th;
    volatile bool running;
    bool joined;
} bb_timer_impl_t;

static void *timer_thread(void *arg)
{
    bb_timer_impl_t *impl = (bb_timer_impl_t *)arg;

    while (1) {
        struct timespec ts = {
            .tv_sec = impl->period_us / 1000000,
            .tv_nsec = (impl->period_us % 1000000) * 1000
        };
        nanosleep(&ts, NULL);

        if (!impl->running) {
            break;
        }

        impl->cb(impl->arg);

        if (impl->type == BB_TIMER_ONE_SHOT) {
            break;
        }
    }

    return NULL;
}

bb_err_t bb_timer_create(const char *name, bb_timer_type_t type,
                         uint64_t period_us, bb_timer_cb_t cb, void *arg,
                         bb_timer_handle_t *out)
{
    (void)name;  // Unused on host

    if (out == NULL || cb == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)malloc(sizeof(bb_timer_impl_t));
    if (impl == NULL) {
        return BB_ERR_NO_SPACE;
    }

    impl->type = type;
    impl->period_us = period_us;
    impl->cb = cb;
    impl->arg = arg;
    impl->running = false;
    impl->joined = false;

    *out = impl;
    return BB_OK;
}

bb_err_t bb_timer_start(bb_timer_handle_t h)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)h;

    impl->running = true;
    if (pthread_create(&impl->th, NULL, timer_thread, impl) != 0) {
        impl->running = false;
        return BB_ERR_INVALID_STATE;
    }

    return BB_OK;
}

bb_err_t bb_timer_stop(bb_timer_handle_t h)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)h;

    impl->running = false;

    if (!impl->joined) {
        pthread_join(impl->th, NULL);
        impl->joined = true;
    }

    return BB_OK;
}

bb_err_t bb_timer_delete(bb_timer_handle_t h)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    bb_timer_impl_t *impl = (bb_timer_impl_t *)h;

    if (!impl->joined) {
        bb_timer_stop(h);
    }

    free(impl);
    return BB_OK;
}
