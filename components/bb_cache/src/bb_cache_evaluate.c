// Pure age classifier for bb_cache's opt-in AGE_OUT eviction policy
// (B1-592 A3). NO locks, NO clock reads, NO I/O -- shared verbatim between
// the LAZY (read-time) and SWEEP (periodic backstop) eviction paths in
// platform/espidf/bb_cache/bb_cache_espidf.c, and directly host-testable in
// isolation (see test/test_host/test_bb_cache_evaluate.c).
//
// THIN WRAPPER (B1-1031): the actual classification logic lives in
// bb_core's bb_age_classify() -- shared with bb_queue's age-eviction. This
// function only maps bb_age_state_t onto bb_cache's own public
// bb_cache_entry_state_t enum, which stays unchanged for back-compat.

#include "bb_cache.h"
#include "bb_age.h"

bb_cache_entry_state_t bb_cache_evaluate_age(uint64_t age_ms, uint32_t stale_age_ms,
                                              uint32_t evict_age_ms)
{
    switch (bb_age_classify(age_ms, stale_age_ms, evict_age_ms)) {
    case BB_AGE_EVICT: return BB_CACHE_ENTRY_EVICT;
    case BB_AGE_STALE: return BB_CACHE_ENTRY_STALE;
    case BB_AGE_FRESH:
    default:           return BB_CACHE_ENTRY_FRESH;
    }
}
