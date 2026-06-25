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

bb_err_t bb_claim_acquire(bb_claim_t *c, const char *id)
{
    if (!c || !id) return BB_ERR_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    if (c->holder == NULL) {
        // Slot free — take it.
        c->holder = id;
        pthread_mutex_unlock(&c->lock);
        return BB_OK;
    }

    // Idempotent: same id (pointer equality OR strcmp).
    if (c->holder == id || strcmp(c->holder, id) == 0) {
        pthread_mutex_unlock(&c->lock);
        return BB_OK;
    }

    // Held by a different id — conflict.
    BB_CLAIM_LOGW(TAG, "claim conflict: '%s' holds slot, '%s' rejected", c->holder, id);
    pthread_mutex_unlock(&c->lock);
    return BB_ERR_CONFLICT;
}

void bb_claim_release(bb_claim_t *c, const char *id)
{
    if (!c || !id) return;

    pthread_mutex_lock(&c->lock);

    if (c->holder != NULL &&
        (c->holder == id || strcmp(c->holder, id) == 0)) {
        c->holder = NULL;
    }
    // No-op if free or held by someone else.

    pthread_mutex_unlock(&c->lock);
}

const char *bb_claim_holder(bb_claim_t *c)
{
    if (!c) return NULL;

    pthread_mutex_lock(&c->lock);
    const char *h = c->holder;
    pthread_mutex_unlock(&c->lock);
    return h;
}

#ifdef BB_CLAIM_TESTING
void bb_claim_reset(bb_claim_t *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    c->holder = NULL;
    pthread_mutex_unlock(&c->lock);
}
#endif
