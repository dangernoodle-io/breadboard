// bb_meminfo_heap_state — pure heap-state classifier.
//
// No ESP-IDF dependencies; compiles on host and device. Relocated verbatim
// from bb_board (B1-977 dissolution) — see bb_meminfo.h for threshold
// constants.
#include "bb_meminfo.h"

// Heap threshold sanity: CRITICAL must be below LOW or the CRITICAL bucket is
// unreachable (bb_meminfo_classify_heap would return LOW before CRITICAL).
_Static_assert(BB_MEMINFO_HEAP_CRITICAL_BYTES < BB_MEMINFO_HEAP_LOW_BYTES,
    "BB_MEMINFO_HEAP_CRITICAL_BYTES must be < BB_MEMINFO_HEAP_LOW_BYTES "
    "(lower CONFIG_BB_MEMINFO_HEAP_CRITICAL_BYTES or raise "
    "CONFIG_BB_MEMINFO_HEAP_LOW_BYTES)");

// ---------------------------------------------------------------------------
// Heap state (module static; zero-init = BB_MEMINFO_HEAP_STATE_OK)
// ---------------------------------------------------------------------------

static bb_meminfo_heap_state_t s_heap_state = BB_MEMINFO_HEAP_STATE_OK;

bb_meminfo_heap_state_t bb_meminfo_classify_heap(size_t free_bytes)
{
    if (free_bytes < (size_t)BB_MEMINFO_HEAP_CRITICAL_BYTES) {
        return BB_MEMINFO_HEAP_STATE_CRITICAL;
    }
    if (free_bytes < (size_t)BB_MEMINFO_HEAP_LOW_BYTES) {
        return BB_MEMINFO_HEAP_STATE_LOW;
    }
    return BB_MEMINFO_HEAP_STATE_OK;
}

bb_meminfo_heap_state_t bb_meminfo_heap_state(void)
{
    return s_heap_state;
}

// Internal setter — not declared in the public header; a caller forward-
// declares it with extern. No production caller exists yet — awaits a
// future periodic heap-evaluator call site.
void bb_meminfo_set_heap_state(bb_meminfo_heap_state_t state)
{
    s_heap_state = state;
}

const char *bb_meminfo_heap_state_str(bb_meminfo_heap_state_t state)
{
    switch (state) {
    case BB_MEMINFO_HEAP_STATE_OK:       return "ok";
    case BB_MEMINFO_HEAP_STATE_LOW:      return "low";
    case BB_MEMINFO_HEAP_STATE_CRITICAL: return "critical";
    default:                             return "ok";
    }
}
