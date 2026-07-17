#pragma once

// bb_age -- pure age classifier shared by any component that needs a
// FRESH/STALE/EVICT decision from an elapsed-age value against two
// configured windows (a stale-only warning window and a hard evict
// window). NO locks, NO clock reads, NO I/O -- lifted out of bb_cache's
// AGE_OUT eviction policy (B1-592 A3) so bb_queue's age-eviction (B1-1031)
// and any future consumer share one classifier instead of re-hand-rolling
// the same boundary logic.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_AGE_FRESH,
    BB_AGE_STALE,
    BB_AGE_EVICT,
} bb_age_state_t;

// Classify age_ms against the configured windows:
//   age_ms <  stale_age_ms                       -> FRESH
//   stale_age_ms <= age_ms < evict_age_ms         -> STALE
//   age_ms >= evict_age_ms                        -> EVICT
// stale_age_ms == 0 means "no stale window" -- the value stays FRESH until
// it crosses evict_age_ms (never reports STALE).
bb_age_state_t bb_age_classify(uint64_t age_ms, uint32_t stale_age_ms,
                                uint32_t evict_age_ms);

#ifdef __cplusplus
}
#endif
