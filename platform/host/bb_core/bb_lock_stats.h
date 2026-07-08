#pragma once
// bb_lock_stats — private helper shared by both bb_lock backends
// (platform/host/bb_core/bb_lock.c and platform/espidf/bb_core/bb_lock.c).
//
// The two backends' acquire/release stats bookkeeping (contention/wait/hold
// accounting + CAS-max loops) was byte-identical; this header + its
// compiled-once-per-platform TU (bb_lock_stats.c) is the single copy both
// backends call into. Private — not part of the public bb_core surface, not
// under components/bb_core/include/.
#include "bb_lock.h"

#if BB_LOCK_STATS_ENABLE

// Record a completed acquire. wait_us == 0 means an uncontended (fast-path)
// acquire; a caller passing a non-zero wait_us must also have already
// incremented lock->bb_lock_contention_count itself (this fn does not).
void bb_lock_stats_record_acquired(bb_lock_t *lock, uint64_t wait_us);

// Record a release. No-op if the lock's held-since timestamp is already 0
// (i.e. the matching acquire was never instrumented).
void bb_lock_stats_record_released(bb_lock_t *lock);

#endif // BB_LOCK_STATS_ENABLE
