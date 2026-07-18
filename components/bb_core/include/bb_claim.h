#pragma once
#include "bb_core.h"
#include "bb_lock.h"
#include "bb_once.h"

#ifdef __cplusplus
extern "C" {
#endif

// bb_claim — portable non-blocking exclusive-slot arbiter.
//
// A bb_claim_t tracks which named holder currently owns a slot. Acquire is
// idempotent for the same id and non-blocking: a different holder gets
// BB_ERR_CONFLICT immediately (no blocking).
//
// Declare file-scope instances with BB_CLAIM_INIT; no explicit init call
// needed — the underlying bb_lock_t is lazily bb_lock_init()'d (via
// bb_once_run) on first use, so no platform mutex header/type appears here.

typedef struct {
    const char *holder;
    bb_lock_t   lock;
    bb_once_t   lock_once;
} bb_claim_t;

// File-scope/static initializer ONLY — no bb_claim_init() call needed;
// unspecified members (lock) are zero-initialized, matching bb_lock_t's own
// never-bb_lock_init()'d-yet zero state. Do NOT instantiate a bb_claim_t as
// a stack/automatic-storage local: the lazily bb_lock_init()'d lock has no
// bb_claim_reset() destroy path, so a local going out of scope leaks the
// underlying platform lock resource.
#define BB_CLAIM_INIT { .holder = NULL, .lock_once = BB_ONCE_INIT }

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
