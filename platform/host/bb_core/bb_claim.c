// bb_claim — portable non-blocking exclusive-slot arbiter.
// Compiled on both host (tests) and ESP-IDF (same object file via CMake SRCS).
// Does NOT include bb_log.h — bb_core has no bb_log dependency; use the
// platform logging primitive directly to stay self-contained.

#include "bb_claim.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define BB_CLAIM_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define BB_CLAIM_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s): " fmt "\n", tag, ##__VA_ARGS__)
#endif

static const char *TAG = "bb_claim";

// Lazily bb_lock_init() c->lock exactly once (bb_once_run) — mirrors
// BB_CLAIM_INIT's "no explicit init call needed" contract while keeping the
// platform mutex type out of bb_claim.h.
static void claim_init_lock(void *ctx)
{
    bb_claim_t *c = ctx;
    bb_lock_config_t cfg = { .name = "bb_claim" };
    bb_lock_init(&cfg, &c->lock);
}

static inline void claim_ensure_lock(bb_claim_t *c)
{
    bb_once_run(&c->lock_once, claim_init_lock, c);
}

bb_err_t bb_claim_acquire(bb_claim_t *c, const char *id)
{
    if (!c || !id) return BB_ERR_INVALID_ARG;

    claim_ensure_lock(c);
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

    claim_ensure_lock(c);
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

    claim_ensure_lock(c);
    bb_lock_lock(&c->lock);
    const char *h = c->holder;
    bb_lock_unlock(&c->lock);
    return h;
}

#ifdef BB_CLAIM_TESTING
void bb_claim_reset(bb_claim_t *c)
{
    if (!c) return;
    claim_ensure_lock(c);
    bb_lock_lock(&c->lock);
    c->holder = NULL;
    bb_lock_unlock(&c->lock);
}
#endif
