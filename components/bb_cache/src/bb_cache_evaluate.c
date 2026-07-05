// Pure age classifier for bb_cache's opt-in AGE_OUT eviction policy
// (B1-592 A3). NO locks, NO clock reads, NO I/O -- shared verbatim between
// the LAZY (read-time) and SWEEP (periodic backstop) eviction paths in
// platform/espidf/bb_cache/bb_cache_espidf.c, and directly host-testable in
// isolation (see test/test_host/test_bb_cache_evaluate.c).

#include "bb_cache.h"

bb_cache_entry_state_t bb_cache_evaluate_age(uint64_t age_ms, uint32_t stale_age_ms,
                                              uint32_t evict_age_ms)
{
    if (age_ms >= (uint64_t)evict_age_ms) return BB_CACHE_ENTRY_EVICT;
    if (stale_age_ms == 0) return BB_CACHE_ENTRY_FRESH;  // no stale window
    if (age_ms >= (uint64_t)stale_age_ms) return BB_CACHE_ENTRY_STALE;
    return BB_CACHE_ENTRY_FRESH;
}
