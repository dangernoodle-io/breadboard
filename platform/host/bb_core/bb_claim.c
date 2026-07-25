// bb_claim — portable non-blocking exclusive-slot arbiter.
// Compiled on both host (tests) and ESP-IDF (same object file via CMake SRCS).
// Does NOT include bb_log.h — bb_core has no bb_log dependency; use the
// platform logging primitive directly to stay self-contained.

#include "bb_claim.h"
#include "bb_lock_once.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define BB_CLAIM_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define BB_CLAIM_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s): " fmt "\n", tag, ##__VA_ARGS__)
#endif

static const char *TAG = "bb_claim";

// Lazily bb_lock_init() c->lock exactly once via bb_lock_once_ensure() —
// mirrors BB_CLAIM_INIT's "no explicit init call needed" contract while
// keeping the platform mutex type out of bb_claim.h. Unlike the plain
// bb_once_run() this replaced, a transient bb_lock_init() failure resets to
// IDLE instead of latching DONE forever, so a later caller genuinely retries
// (B1-1203, mirrors B1-524).
static inline bb_err_t claim_ensure_lock(bb_claim_t *c)
{
    bb_lock_config_t cfg = { .name = "bb_claim" };
    return bb_lock_once_ensure(&c->lock_once, &cfg, &c->lock);
}

bb_err_t bb_claim_acquire(bb_claim_t *c, const char *id)
{
    if (!c || !id) return BB_ERR_INVALID_ARG;

    bb_err_t lock_rc = claim_ensure_lock(c);
    // LCOV_EXCL_START -- bb_lock_init() failure is not host-reproducible
    // (same rationale as bb_lock_once.c's own LCOV_EXCL comment); this is a
    // defensive path, not a real branch the test suite can drive. HIL-only
    // equivalent tracked as B1-692.
    if (lock_rc != BB_OK) {
        BB_CLAIM_LOGW(TAG, "acquire('%s'): lock unavailable", id);
        return lock_rc;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&c->lock);

    if (c->holder == NULL) {
        // Slot free — take it.
        c->holder = id;
        bb_lock_unlock(&c->lock);
        return BB_OK;
    }

    // Idempotent: same id (pointer equality OR strcmp).
    if (c->holder == id || strcmp(c->holder, id) == 0) {
        bb_lock_unlock(&c->lock);
        return BB_OK;
    }

    // Held by a different id — conflict.
    BB_CLAIM_LOGW(TAG, "claim conflict: '%s' holds slot, '%s' rejected", c->holder, id);
    bb_lock_unlock(&c->lock);
    return BB_ERR_CONFLICT;
}

void bb_claim_release(bb_claim_t *c, const char *id)
{
    if (!c || !id) return;

    // LCOV_EXCL_START -- bb_lock_init() failure is not host-reproducible;
    // see bb_claim_acquire()'s shared rationale above. Also, when it is
    // reached in practice, no claim could ever have been acquired either, so
    // there is nothing to release.
    if (claim_ensure_lock(c) != BB_OK) {
        return;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&c->lock);

    if (c->holder != NULL &&
        (c->holder == id || strcmp(c->holder, id) == 0)) {
        c->holder = NULL;
    }
    // No-op if free or held by someone else.

    bb_lock_unlock(&c->lock);
}

const char *bb_claim_holder(bb_claim_t *c)
{
    if (!c) return NULL;

    // LCOV_EXCL_START -- bb_lock_init() failure is not host-reproducible;
    // see bb_claim_acquire()'s shared rationale above.
    if (claim_ensure_lock(c) != BB_OK) {
        return NULL;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&c->lock);
    const char *h = c->holder;
    bb_lock_unlock(&c->lock);
    return h;
}

#ifdef BB_CLAIM_TESTING
void bb_claim_reset(bb_claim_t *c)
{
    if (!c) return;
    // LCOV_EXCL_START -- bb_lock_init() failure is not host-reproducible;
    // see bb_claim_acquire()'s shared rationale above.
    if (claim_ensure_lock(c) != BB_OK) {
        return;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&c->lock);
    c->holder = NULL;
    bb_lock_unlock(&c->lock);
}
#endif
