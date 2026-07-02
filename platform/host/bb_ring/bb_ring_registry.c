// bb_ring_registry — thin consumer of the generic bb_registry primitive.
// Compiled on both host (tests) and ESP-IDF as part of the bb_ring component.

#include "bb_ring_registry.h"
#include "bb_registry.h"
#include "bb_log.h"

#include <pthread.h>

/* Kconfig bridge: honour CONFIG_BB_RING_REGISTRY_MAX from build flags; default 16. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_RING_REGISTRY_MAX
#define BB_RING_REGISTRY_MAX CONFIG_BB_RING_REGISTRY_MAX
#endif
#ifndef BB_RING_REGISTRY_MAX
#define BB_RING_REGISTRY_MAX 16
#endif

static const char *TAG = "bb_ring_registry";

BB_REGISTRY_DEFINE_TAGGED(s_ring_registry, BB_RING_REGISTRY_MAX, "rings");

// Wrapper mutex — serialises the four public ops (register/deregister/
// foreach/count) as a single atomic unit over the top of the primitive's own
// internal lock. Two concurrency holes this closes (both HIGH severity):
//
//   1. deregister TOCTOU — resolving name-by-value then deregistering by
//      name were two separate lock acquisitions on the primitive; a
//      concurrent register() for the same (now-vacated) name between them
//      could steal the slot. Holding s_ring_reg_lock across the whole
//      find+deregister sequence makes it atomic.
//   2. diag-read UAF — bb_ring_destroy() deregisters then frees the ring.
//      A concurrent /api/diag/rings read iterating via foreach could still
//      be dereferencing the ring's fields (bb_ring_count/dropped/...) after
//      it was deregistered-and-freed. Holding s_ring_reg_lock across the
//      ENTIRE foreach call (including the caller's callback) means
//      bb_ring_registry_deregister() blocks on this same lock until the
//      read finishes, so bb_ring_destroy() cannot free the ring out from
//      under a concurrent diag read. This requires bb_ring_destroy() to
//      deregister BEFORE freeing r (it does — see bb_ring.c).
//
// Lock order: s_ring_reg_lock is always taken OUTSIDE any bb_registry_*
// call (the primitive never calls back into this file), so there is no
// lock-order inversion / deadlock risk from the double-locking.
//
// Foreach contract (see bb_ring_registry.h): the caller's callback runs
// while s_ring_reg_lock is held, so it MUST be bounded, allocation-free,
// non-blocking, and MUST NOT call back into any bb_ring_registry_*
// function (self-deadlock — s_ring_reg_lock is not recursive).
static pthread_mutex_t s_ring_reg_lock = PTHREAD_MUTEX_INITIALIZER;

bb_err_t bb_ring_registry_register(const char *name, bb_ring_t r)
{
    if (!name || !r) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&s_ring_reg_lock);
    bb_err_t err = bb_registry_register(&s_ring_registry, name, (void *)r);
    pthread_mutex_unlock(&s_ring_reg_lock);

    if (err != BB_OK) {
        bb_log_w(TAG, "register('%s') failed: %d", name, (int)err);
    }
    return err;
}

// find_by_value scan state — used by bb_ring_registry_deregister below.
typedef struct {
    void       *target;
    const char *found_name;
} find_by_value_t;

static void find_by_value_cb(const char *name, void *value, void *ctx)
{
    find_by_value_t *scan = (find_by_value_t *)ctx;
    if (!scan->found_name && value == scan->target) {
        scan->found_name = name;
    }
}

bb_err_t bb_ring_registry_deregister(bb_ring_t r)
{
    if (!r) {
        return BB_ERR_INVALID_ARG;
    }

    // The primitive deregisters by name; resolve the name by scanning for
    // the matching value via foreach (option (c) from the design doc — the
    // least invasive: no bb_ring.h dependency, no extra field on bb_ring_t).
    // The whole find+deregister sequence runs under s_ring_reg_lock so it is
    // atomic with respect to concurrent register()/deregister()/foreach()
    // calls (closes the TOCTOU where a same-name register() could steal the
    // slot between the scan and the deregister).
    pthread_mutex_lock(&s_ring_reg_lock);

    find_by_value_t scan = { .target = (void *)r, .found_name = NULL };
    bb_registry_foreach(&s_ring_registry, find_by_value_cb, &scan);

    bb_err_t err;
    if (!scan.found_name) {
        err = BB_ERR_NOT_FOUND;
    } else {
        err = bb_registry_deregister(&s_ring_registry, scan.found_name);
    }

    pthread_mutex_unlock(&s_ring_reg_lock);
    return err;
}

uint16_t bb_ring_registry_count(void)
{
    pthread_mutex_lock(&s_ring_reg_lock);
    uint16_t count = bb_registry_count(&s_ring_registry);
    pthread_mutex_unlock(&s_ring_reg_lock);
    return count;
}

// Trampoline: bridges bb_registry_foreach's (name, void*, ctx) callback shape
// to bb_ring_registry_foreach's (name, bb_ring_t, ctx) shape without an
// incompatible function-pointer cast.
typedef struct {
    bb_ring_registry_cb_t cb;
    void                 *ctx;
} foreach_bridge_t;

static void foreach_trampoline(const char *name, void *value, void *ctx)
{
    foreach_bridge_t *bridge = (foreach_bridge_t *)ctx;
    bridge->cb(name, (bb_ring_t)value, bridge->ctx);
}

void bb_ring_registry_foreach(bb_ring_registry_cb_t cb, void *ctx)
{
    if (!cb) {
        return;
    }
    // s_ring_reg_lock is held across the ENTIRE call, including every
    // invocation of the caller's cb — see the UAF note on s_ring_reg_lock
    // above. bb_ring_destroy() cannot proceed past its deregister call
    // (also gated on s_ring_reg_lock) until this returns.
    pthread_mutex_lock(&s_ring_reg_lock);
    foreach_bridge_t bridge = { .cb = cb, .ctx = ctx };
    bb_registry_foreach(&s_ring_registry, foreach_trampoline, &bridge);
    pthread_mutex_unlock(&s_ring_reg_lock);
}

#ifdef BB_RING_REGISTRY_TESTING
void bb_ring_registry_test_reset(void)
{
    pthread_mutex_lock(&s_ring_reg_lock);
    bb_registry_reset(&s_ring_registry);
    pthread_mutex_unlock(&s_ring_reg_lock);
}
#endif
