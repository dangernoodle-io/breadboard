// Pure age classifier -- see bb_age.h. NO locks, NO clock reads, NO I/O.

#include "bb_age.h"

bb_age_state_t bb_age_classify(uint64_t age_ms, uint32_t stale_age_ms,
                                uint32_t evict_age_ms)
{
    if (age_ms >= (uint64_t)evict_age_ms) return BB_AGE_EVICT;
    if (stale_age_ms == 0) return BB_AGE_FRESH;  // no stale window
    if (age_ms >= (uint64_t)stale_age_ms) return BB_AGE_STALE;
    return BB_AGE_FRESH;
}
