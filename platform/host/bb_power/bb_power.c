// bb_power dispatch layer — platform-independent; shared by espidf and host.
// Mutex approach: bb_lock_t (bb_core) — its host backend wraps pthread_mutex_t,
// its ESP-IDF backend a FreeRTOS semaphore; this file never sees either
// platform type directly.
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_json.h"
#include "bb_lock.h"
#include <stdlib.h>

struct bb_power {
    const bb_power_driver_t *drv;
    void *state;
    bb_power_snapshot_t cache;
    bb_lock_t lock;
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
    bb_lock_init(NULL, &h->lock);
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

    BB_LOCKED_COPY(&h->lock, h->cache, s);

    if (h->drv->poll) h->drv->poll(h->state);

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
    BB_LOCKED_COPY(&h->lock, *out, h->cache);
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

void *bb_power_handle_state(bb_power_handle_t h)
{
    if (!h) return NULL;
    return h->state;
}

void bb_power_set_primary(bb_power_handle_t h) { s_primary = h; }
bb_power_handle_t bb_power_primary(void)        { return s_primary; }

// ---------------------------------------------------------------------------
// JSON serializer — single builder used by REST responses.
// ---------------------------------------------------------------------------

void bb_power_emit(bb_json_t obj, const bb_power_snapshot_t *snap)
{
    if (!obj || !snap) return;

    if (snap->vout_mv >= 0) {
        bb_json_obj_set_number(obj, "vout_mv", (double)snap->vout_mv);
    } else {
        bb_json_obj_set_null(obj, "vout_mv");
    }

    if (snap->iout_ma >= 0) {
        bb_json_obj_set_number(obj, "iout_ma", (double)snap->iout_ma);
    } else {
        bb_json_obj_set_null(obj, "iout_ma");
    }

    if (snap->pout_mw >= 0) {
        bb_json_obj_set_number(obj, "pout_mw", (double)snap->pout_mw);
    } else {
        bb_json_obj_set_null(obj, "pout_mw");
    }

    if (snap->vin_mv >= 0) {
        bb_json_obj_set_number(obj, "vin_mv", (double)snap->vin_mv);
    } else {
        bb_json_obj_set_null(obj, "vin_mv");
    }

    if (snap->temp_c >= 0) {
        bb_json_obj_set_number(obj, "temp_c", (double)snap->temp_c);
    } else {
        bb_json_obj_set_null(obj, "temp_c");
    }
}

// ---------------------------------------------------------------------------
// Shared emit helper — writes "present" plus power fields. SSOT for the
// /api/sensors power section (bb_sensors); folded from the former
// bb_power_routes glue component (its emit_section was a pure passthrough).
// ---------------------------------------------------------------------------

void bb_power_emit_section(bb_json_t obj)
{
    bb_power_handle_t h = bb_power_primary();
    bb_power_snapshot_t snap;
    bb_power_snapshot(h, &snap);
    bb_json_obj_set_bool(obj, "present", h != NULL);
    bb_power_emit(obj, &snap);
}

#ifdef BB_POWER_TESTING
#include "bb_power_test.h"
void bb_power_test_reset(void)
{
    s_primary = NULL;
}
#endif
