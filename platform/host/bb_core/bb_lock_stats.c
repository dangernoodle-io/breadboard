// bb_lock_stats — shared acquire/release bookkeeping for both bb_lock
// backends. Compiled once, linked into both the host and ESP-IDF builds
// (host: picked up automatically via the platform/host/<component> source
// glob; ESP-IDF: added explicitly to components/bb_core/CMakeLists.txt SRCS
// — same cross-platform-shared-TU pattern as bb_claim.c in this directory).

#include "bb_lock_stats.h"

#if BB_LOCK_STATS_ENABLE

#include "bb_clock.h"
#include <stdatomic.h>

void bb_lock_stats_record_acquired(bb_lock_t *lock, uint64_t wait_us)
{
    atomic_fetch_add_explicit(&lock->bb_lock_acquisition_count, 1u, memory_order_relaxed);
    if (wait_us > 0) {
        atomic_fetch_add_explicit(&lock->bb_lock_wait_time_total_us, wait_us, memory_order_relaxed);
        uint64_t cur_max = atomic_load_explicit(&lock->bb_lock_wait_time_max_us, memory_order_relaxed);
        // Retry loop: only re-enters when a concurrent updater's CAS races
        // this one between the load above and the CAS below. Host tests
        // exercise a single contended waiter per lock, so the retry path
        // (loop re-check + CAS-fail branch) is defensive-but-unreachable
        // without a second concurrent writer racing the exact same window;
        // not chased here (HIL-only equivalent tracked as B1-692).
        while (wait_us > cur_max) {  // LCOV_EXCL_BR_LINE — retry branch, see comment above
            if (atomic_compare_exchange_weak_explicit(&lock->bb_lock_wait_time_max_us, &cur_max, wait_us,  // LCOV_EXCL_BR_LINE — CAS-fail retry branch, see comment above
                                                       memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        }
    }
    atomic_store_explicit(&lock->bb_lock_held_since_us, bb_clock_now_us(), memory_order_relaxed);
}

void bb_lock_stats_record_released(bb_lock_t *lock)
{
    uint64_t since = atomic_exchange_explicit(&lock->bb_lock_held_since_us, 0, memory_order_relaxed);
    if (since == 0) {
        return;
    }
    uint64_t hold_us = bb_clock_now_us() - since;
    atomic_fetch_add_explicit(&lock->bb_lock_hold_time_total_us, hold_us, memory_order_relaxed);
    uint64_t cur_max = atomic_load_explicit(&lock->bb_lock_hold_time_max_us, memory_order_relaxed);
    while (hold_us > cur_max) {
        // CAS-fail retry branch: only taken when a concurrent updater races
        // this CAS between the load above and here — not host-reproducible
        // with a single-writer test lock; see the wait_time_max_us retry
        // loop comment above (HIL-only equivalent tracked as B1-692).
        if (atomic_compare_exchange_weak_explicit(&lock->bb_lock_hold_time_max_us, &cur_max, hold_us,  // LCOV_EXCL_BR_LINE — CAS-fail retry branch, see comment above
                                                   memory_order_relaxed, memory_order_relaxed)) {
            break;
        }
    }
}

#endif // BB_LOCK_STATS_ENABLE
