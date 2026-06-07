// bb_power dispatch layer — platform-independent; shared by espidf and host.
// Mutex approach: pthread_mutex_t on host; on ESP-IDF pthread.h is also
// available (ESP-IDF ships a POSIX pthread layer), so a single
// PTHREAD_MUTEX_INITIALIZER works on both targets.
#include "bb_power.h"
#include "bb_power_driver.h"
#include <stdlib.h>
#include <pthread.h>

struct bb_power {
    const bb_power_driver_t *drv;
    void *state;
    bb_power_snapshot_t cache;
    pthread_mutex_t lock;
};

static bb_power_handle_t s_primary = NULL;

bb_err_t bb_power_handle_create(const bb_power_driver_t *drv, void *state,
                                 bb_power_handle_t *out)
{
    if (!drv || !out) return BB_ERR_INVALID_ARG;
    bb_power_handle_t h = (bb_power_handle_t)calloc(1, sizeof(struct bb_power));
    if (!h) return BB_ERR_NO_SPACE; // LCOV_EXCL_LINE
    h->drv   = drv;
    h->state = state;
    // All snapshot fields start at -1 (unavailable) until first poll.
    h->cache.vout_mv = -1;
    h->cache.iout_ma = -1;
    h->cache.pout_mw = -1;
    h->cache.vin_mv  = -1;
    h->cache.temp_c  = -1;
    pthread_mutex_init(&h->lock, NULL);
    *out = h;
    return BB_OK;
}

bb_err_t bb_power_poll(bb_power_handle_t h)
{
    if (!h || !h->drv) return BB_ERR_INVALID_STATE;

    bb_power_snapshot_t s;
    s.vout_mv = h->drv->read_vout_mv ? h->drv->read_vout_mv(h->state) : -1;
    s.iout_ma = h->drv->read_iout_ma ? h->drv->read_iout_ma(h->state) : -1;
    s.vin_mv  = h->drv->read_vin_mv  ? h->drv->read_vin_mv(h->state)  : -1;
    s.temp_c  = h->drv->read_temp_c  ? h->drv->read_temp_c(h->state)  : -1;

    // pout_mw = vout_mv * iout_ma / 1000 when both readings are valid (≥0).
    if (s.vout_mv >= 0 && s.iout_ma >= 0) {
        s.pout_mw = (int)(((int64_t)s.vout_mv * s.iout_ma) / 1000);
    } else {
        s.pout_mw = -1;
    }

    pthread_mutex_lock(&h->lock);
    h->cache = s;
    pthread_mutex_unlock(&h->lock);

    return BB_OK;
}

void bb_power_snapshot(bb_power_handle_t h, bb_power_snapshot_t *out)
{
    if (!out) return;
    if (!h || !h->drv) {
        out->vout_mv = -1;
        out->iout_ma = -1;
        out->pout_mw = -1;
        out->vin_mv  = -1;
        out->temp_c  = -1;
        return;
    }
    pthread_mutex_lock(&h->lock);
    *out = h->cache;
    pthread_mutex_unlock(&h->lock);
}

bb_err_t bb_power_set_vout_mv(bb_power_handle_t h, uint16_t mv)
{
    if (!h || !h->drv) return BB_ERR_INVALID_STATE;
    if (!h->drv->set_vout_mv) return BB_ERR_UNSUPPORTED;
    return h->drv->set_vout_mv(h->state, mv);
}

const char *bb_power_name(bb_power_handle_t h)
{
    if (!h || !h->drv) return NULL;
    return h->drv->name;
}

void bb_power_set_primary(bb_power_handle_t h) { s_primary = h; }
bb_power_handle_t bb_power_primary(void)        { return s_primary; }

#ifdef BB_POWER_TESTING
#include "bb_power_test.h"
void bb_power_test_reset(void)
{
    s_primary = NULL;
}
#endif
