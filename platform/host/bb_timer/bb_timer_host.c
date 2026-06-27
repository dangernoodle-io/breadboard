#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "bb_timer.h"
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>

/* -------------------------------------------------------------------------
 * All includes at file top — conditional use guarded at call sites
 * -------------------------------------------------------------------------*/
#ifdef BB_TIMER_TESTING
#include "bb_timer_periodic_test.h"
#include "bb_timer_oneshot_test.h"
#include "bb_timer_deferred_test.h"
#endif

/* Internal mode constants */
#define BB_TIMER_MODE_DIRECT  0
#define BB_TIMER_MODE_SHARED  1
#define BB_TIMER_MODE_WORKER  2

/* -------------------------------------------------------------------------
 * Injectable malloc for testing (covers deferred periodic + oneshot + worker)
 * -------------------------------------------------------------------------*/
#ifdef BB_TIMER_TESTING
static void *(*s_disp_malloc)(size_t) = NULL;
static void *host_disp_malloc(size_t n) { return s_disp_malloc ? s_disp_malloc(n) : malloc(n); }
#else
static void *host_disp_malloc(size_t n) { return malloc(n); }
#endif

/* -------------------------------------------------------------------------
 * Sync-mode flag (toggled by test helpers)
 * In sync mode, fire_for_test helpers call work_fn inline.
 * -------------------------------------------------------------------------*/
static bool s_sync_mode = false;

#ifdef BB_TIMER_TESTING
void bb_timer_host_set_sync_mode(bool v) { s_sync_mode = v; }
#endif

/* -------------------------------------------------------------------------
 * Forward declarations of struct types
 * -------------------------------------------------------------------------*/
struct bb_periodic_timer;
struct bb_oneshot_timer;

/* -------------------------------------------------------------------------
 * Generic low-level timer (bb_timer_create/start/stop/delete)
 * -------------------------------------------------------------------------*/

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
            .tv_sec  = impl->period_us / 1000000,
            .tv_nsec = (impl->period_us % 1000000) * 1000
        };
        nanosleep(&ts, NULL);
        if (!impl->running) break;
        impl->cb(impl->arg);
        if (impl->type == BB_TIMER_ONE_SHOT) break;
    }
    return NULL;
}

bb_err_t bb_timer_create(const char *name, bb_timer_type_t type,
                         uint64_t period_us, bb_timer_cb_t cb, void *arg,
                         bb_timer_handle_t *out)
{
    (void)name;
    if (out == NULL || cb == NULL) return BB_ERR_INVALID_ARG;
    bb_timer_impl_t *impl = (bb_timer_impl_t *)malloc(sizeof(bb_timer_impl_t));
    if (impl == NULL) return BB_ERR_NO_SPACE;
    impl->type      = type;
    impl->period_us = period_us;
    impl->cb        = cb;
    impl->arg       = arg;
    impl->running   = false;
    impl->joined    = false;
    *out = impl;
    return BB_OK;
}

bb_err_t bb_timer_start(bb_timer_handle_t h)
{
    if (h == NULL) return BB_ERR_INVALID_ARG;
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
    if (h == NULL) return BB_ERR_INVALID_ARG;
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
    if (h == NULL) return BB_ERR_INVALID_ARG;
    bb_timer_impl_t *impl = (bb_timer_impl_t *)h;
    if (!impl->joined) bb_timer_stop(h);
    free(impl);
    return BB_OK;
}

uint64_t bb_timer_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* -------------------------------------------------------------------------
 * Struct definitions (full)
 * -------------------------------------------------------------------------*/

struct bb_periodic_timer {
    void     (*cb)(void *arg);
    void      *arg;
    uint64_t   period_us;
    bool       running;
    /* Deferred fields */
    void       (*work_fn)(void *arg);
    const char  *name;
    int          mode;
    volatile bool pending;
};

struct bb_oneshot_timer {
    void     (*cb)(void *arg);
    void      *arg;
    uint64_t   delay_us;
    bool       armed;
    /* Deferred fields */
    void       (*work_fn)(void *arg);
    const char  *name;
    int          mode;
    volatile bool pending;
};

/* -------------------------------------------------------------------------
 * Periodic timer API — host stub
 * -------------------------------------------------------------------------*/

bb_err_t bb_timer_periodic_create(void (*cb)(void *arg), void *arg,
                                  const char *name, bb_periodic_timer_t *out)
{
    (void)name;
    if (cb == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    struct bb_periodic_timer *t =
        (struct bb_periodic_timer *)malloc(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;
    t->cb        = cb;
    t->arg       = arg;
    t->period_us = 0;
    t->running   = false;
    t->work_fn   = NULL;
    t->name      = name;
    t->mode      = BB_TIMER_MODE_DIRECT;
    t->pending   = false;
    *out = t;
    return BB_OK;
}

bb_err_t bb_timer_periodic_start(bb_periodic_timer_t t, uint64_t period_us)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    t->period_us = period_us;
    t->running   = true;
    return BB_OK;
}

bb_err_t bb_timer_periodic_stop(bb_periodic_timer_t t)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    t->running = false;
    return BB_OK;
}

bb_err_t bb_timer_periodic_delete(bb_periodic_timer_t t)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    free(t);
    return BB_OK;
}

#ifdef BB_TIMER_TESTING
void bb_timer_periodic_fire_for_test(bb_periodic_timer_t t)
{
    if (t == NULL || !t->running) return;
    t->cb(t->arg);
}
#endif

/* -------------------------------------------------------------------------
 * One-shot timer API — host stub
 * -------------------------------------------------------------------------*/

bb_err_t bb_timer_oneshot_create(void (*cb)(void *arg), void *arg,
                                 const char *name, bb_oneshot_timer_t *out)
{
    (void)name;
    if (cb == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    struct bb_oneshot_timer *t =
        (struct bb_oneshot_timer *)malloc(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;
    t->cb       = cb;
    t->arg      = arg;
    t->delay_us = 0;
    t->armed    = false;
    t->work_fn  = NULL;
    t->name     = name;
    t->mode     = BB_TIMER_MODE_DIRECT;
    t->pending  = false;
    *out = t;
    return BB_OK;
}

bb_err_t bb_timer_oneshot_start(bb_oneshot_timer_t t, uint64_t delay_us)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    t->delay_us = delay_us;
    t->armed    = true;
    return BB_OK;
}

bb_err_t bb_timer_oneshot_stop(bb_oneshot_timer_t t)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    t->armed   = false;
    t->pending = false;
    return BB_OK;
}

bb_err_t bb_timer_oneshot_delete(bb_oneshot_timer_t t)
{
    if (t == NULL) return BB_ERR_INVALID_ARG;
    free(t);
    return BB_OK;
}

#ifdef BB_TIMER_TESTING
void bb_timer_oneshot_fire_for_test(bb_oneshot_timer_t t)
{
    if (t == NULL || !t->armed) return;
    t->armed = false;
    t->cb(t->arg);
}
#endif

/* -------------------------------------------------------------------------
 * MODE A: Deferred periodic — host stub
 * -------------------------------------------------------------------------*/

bb_err_t bb_timer_deferred_periodic_create(void (*work_fn)(void *arg), void *arg,
                                           const char *name, bb_periodic_timer_t *out)
{
    if (work_fn == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    struct bb_periodic_timer *t =
        (struct bb_periodic_timer *)host_disp_malloc(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;
    t->cb        = NULL;
    t->arg       = arg;
    t->period_us = 0;
    t->running   = false;
    t->work_fn   = work_fn;
    t->name      = name;
    t->mode      = BB_TIMER_MODE_SHARED;
    t->pending   = false;
    *out = t;
    return BB_OK;
}

#ifdef BB_TIMER_TESTING
void bb_timer_deferred_periodic_fire_for_test(bb_periodic_timer_t t)
{
    if (t == NULL || !t->running) return;
    /* Coalesce: if already pending, drop */
    if (t->pending) return;
    t->pending = true;
    if (s_sync_mode) {
        /* Drain synchronously */
        t->pending = false;
        t->work_fn(t->arg);
    }
    /* In non-sync mode: caller drains via bb_timer_deferred_drain_for_test */
}

void bb_timer_deferred_set_pending_for_test(bb_periodic_timer_t t, bool v)
{
    if (t == NULL) return;
    t->pending = v;
}

void bb_timer_deferred_drain_for_test(bb_periodic_timer_t t)
{
    if (t == NULL || !t->running || !t->pending) return;
    t->pending = false;
    t->work_fn(t->arg);
}
#endif

/* -------------------------------------------------------------------------
 * MODE A: Deferred one-shot — host stub
 * -------------------------------------------------------------------------*/

bb_err_t bb_timer_deferred_oneshot_create(void (*work_fn)(void *arg), void *arg,
                                          const char *name, bb_oneshot_timer_t *out)
{
    if (work_fn == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    struct bb_oneshot_timer *t =
        (struct bb_oneshot_timer *)host_disp_malloc(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;
    t->cb       = NULL;
    t->arg      = arg;
    t->delay_us = 0;
    t->armed    = false;
    t->work_fn  = work_fn;
    t->name     = name;
    t->mode     = BB_TIMER_MODE_SHARED;
    t->pending  = false;
    *out = t;
    return BB_OK;
}

#ifdef BB_TIMER_TESTING
void bb_timer_deferred_oneshot_fire_for_test(bb_oneshot_timer_t t)
{
    if (t == NULL || !t->armed) return;
    t->armed   = false;
    t->pending = false;
    t->work_fn(t->arg);
}
#endif

/* -------------------------------------------------------------------------
 * MODE B: Worker timer — host stub (no background thread; synchronous calls)
 * -------------------------------------------------------------------------*/

bb_err_t bb_timer_worker_periodic_create(void (*work_fn)(void *arg), void *arg,
                                         const char *name,
                                         const bb_timer_worker_cfg_t *cfg,
                                         bb_periodic_timer_t *out)
{
    (void)cfg;
    if (work_fn == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    struct bb_periodic_timer *t =
        (struct bb_periodic_timer *)host_disp_malloc(sizeof(*t));
    if (t == NULL) return BB_ERR_NO_SPACE;
    t->cb        = NULL;
    t->arg       = arg;
    t->period_us = 0;
    t->running   = false;
    t->work_fn   = work_fn;
    t->name      = name;
    t->mode      = BB_TIMER_MODE_WORKER;
    t->pending   = false;
    *out = t;
    return BB_OK;
}

#ifdef BB_TIMER_TESTING
void bb_timer_worker_periodic_fire_for_test(bb_periodic_timer_t t)
{
    if (t == NULL) return;
    t->work_fn(t->arg);
}

void bb_timer_set_malloc_for_test(void *(*fn)(size_t))
{
    s_disp_malloc = fn;
}
#endif
