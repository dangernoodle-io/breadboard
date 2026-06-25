#pragma once
#include "bb_core.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// bb_claim — portable non-blocking exclusive-slot arbiter.
//
// A bb_claim_t tracks which named holder currently owns a slot. Acquire is
// idempotent for the same id and non-blocking: a different holder gets
// BB_ERR_CONFLICT immediately (no blocking).
//
// Declare file-scope instances with BB_CLAIM_INIT; no explicit init call needed.

typedef struct {
    const char     *holder;
    pthread_mutex_t lock;
} bb_claim_t;

// Static initializer — no bb_claim_init() call needed for file-scope statics.
#define BB_CLAIM_INIT { .holder = NULL, .lock = PTHREAD_MUTEX_INITIALIZER }

// Acquire the claim for id.
// Returns BB_OK          if the slot was free (now held by id).
// Returns BB_OK          if already held by id (idempotent).
// Returns BB_ERR_CONFLICT if held by a different id (logs a warning).
// Never blocks.
bb_err_t    bb_claim_acquire(bb_claim_t *c, const char *id);

// Release the claim. No-op if held by a different id or if slot is free.
void        bb_claim_release(bb_claim_t *c, const char *id);

// Return the current holder string (NULL if free). Caller must not free it.
const char *bb_claim_holder(bb_claim_t *c);

#ifdef BB_CLAIM_TESTING
// Reset the claim to free. For test isolation only.
void bb_claim_reset(bb_claim_t *c);
#endif

#ifdef __cplusplus
}
#endif
