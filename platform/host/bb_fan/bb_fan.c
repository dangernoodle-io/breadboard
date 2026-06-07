// bb_fan dispatch layer — platform-independent; shared by espidf and host.
// Mutex approach: pthread_mutex_t on host; on ESP-IDF pthread.h is also
// available (ESP-IDF ships a POSIX pthread layer), so a single
// PTHREAD_MUTEX_INITIALIZER works on both targets.
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include <stdlib.h>
#include <math.h>
#include <pthread.h>

struct bb_fan {
    const bb_fan_driver_t *drv;
    void *state;
    bb_fan_snapshot_t cache;
    pthread_mutex_t lock;
};

static bb_fan_handle_t s_primary = NULL;

bb_err_t bb_fan_handle_create(const bb_fan_driver_t *drv, void *state,
                               bb_fan_handle_t *out)
{
    if (!drv || !out) return BB_ERR_INVALID_ARG;
    bb_fan_handle_t h = (bb_fan_handle_t)calloc(1, sizeof(struct bb_fan));
    if (!h) return BB_ERR_NO_SPACE; // LCOV_EXCL_LINE
    h->drv   = drv;
    h->state = state;
    // All snapshot fields start at unavailable until first poll.
    h->cache.rpm      = -1;
    h->cache.duty_pct = -1;
    h->cache.die_c    = NAN;
    h->cache.board_c  = NAN;
    pthread_mutex_init(&h->lock, NULL);
    *out = h;
    return BB_OK;
}

bb_err_t bb_fan_poll(bb_fan_handle_t h)
{
    if (!h || !h->drv) return BB_ERR_INVALID_STATE;

    bb_fan_snapshot_t s;

    s.rpm      = h->drv->read_rpm      ? h->drv->read_rpm(h->state)      : -1;
    s.duty_pct = h->drv->get_duty_pct  ? h->drv->get_duty_pct(h->state)  : -1;

    if (h->drv->read_die_temp_c) {
        float t = NAN;
        s.die_c = (h->drv->read_die_temp_c(h->state, &t) == BB_OK) ? t : NAN;
    } else {
        s.die_c = NAN;
    }

    if (h->drv->read_board_temp_c) {
        float t = NAN;
        s.board_c = (h->drv->read_board_temp_c(h->state, &t) == BB_OK) ? t : NAN;
    } else {
        s.board_c = NAN;
    }

    pthread_mutex_lock(&h->lock);
    h->cache = s;
    pthread_mutex_unlock(&h->lock);

    return BB_OK;
}

void bb_fan_snapshot(bb_fan_handle_t h, bb_fan_snapshot_t *out)
{
    if (!out) return;
    if (!h || !h->drv) {
        out->rpm      = -1;
        out->duty_pct = -1;
        out->die_c    = NAN;
        out->board_c  = NAN;
        return;
    }
    pthread_mutex_lock(&h->lock);
    *out = h->cache;
    pthread_mutex_unlock(&h->lock);
}

bb_err_t bb_fan_set_duty_pct(bb_fan_handle_t h, int pct)
{
    if (!h || !h->drv) return BB_ERR_INVALID_STATE;
    if (!h->drv->set_duty_pct) return BB_ERR_UNSUPPORTED;
    return h->drv->set_duty_pct(h->state, pct);
}

int bb_fan_get_duty_pct(bb_fan_handle_t h)
{
    if (!h || !h->drv) return -1;
    bb_fan_snapshot_t s;
    pthread_mutex_lock(&h->lock);
    s = h->cache;
    pthread_mutex_unlock(&h->lock);
    return s.duty_pct;
}

const char *bb_fan_name(bb_fan_handle_t h)
{
    if (!h || !h->drv) return NULL;
    return h->drv->name;
}

void bb_fan_set_primary(bb_fan_handle_t h) { s_primary = h; }
bb_fan_handle_t bb_fan_primary(void)        { return s_primary; }

#ifdef BB_FAN_TESTING
#include "bb_fan_test.h"
void bb_fan_test_reset(void)
{
    s_primary = NULL;
}
#endif
