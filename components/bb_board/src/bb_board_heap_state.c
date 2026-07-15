// bb_board_heap_state — pure heap-state classifier.
//
// No ESP-IDF dependencies; compiles on host and device. Moved verbatim from
// bb_net_health (net_health teardown PR-C) — see bb_board.h for threshold
// constants.
#include "bb_board.h"

// Heap threshold sanity: CRITICAL must be below LOW or the CRITICAL bucket is
// unreachable (bb_board_classify_heap would return LOW before CRITICAL).
_Static_assert(BB_BOARD_HEAP_CRITICAL_BYTES < BB_BOARD_HEAP_LOW_BYTES,
    "BB_BOARD_HEAP_CRITICAL_BYTES must be < BB_BOARD_HEAP_LOW_BYTES "
    "(lower CONFIG_BB_BOARD_HEAP_CRITICAL_BYTES or raise "
    "CONFIG_BB_BOARD_HEAP_LOW_BYTES)");

// ---------------------------------------------------------------------------
// Heap state (module static; zero-init = BB_BOARD_HEAP_STATE_OK)
// ---------------------------------------------------------------------------

static bb_board_heap_state_t s_heap_state = BB_BOARD_HEAP_STATE_OK;

bb_board_heap_state_t bb_board_classify_heap(size_t free_bytes)
{
    if (free_bytes < (size_t)BB_BOARD_HEAP_CRITICAL_BYTES) {
        return BB_BOARD_HEAP_STATE_CRITICAL;
    }
    if (free_bytes < (size_t)BB_BOARD_HEAP_LOW_BYTES) {
        return BB_BOARD_HEAP_STATE_LOW;
    }
    return BB_BOARD_HEAP_STATE_OK;
}

bb_board_heap_state_t bb_board_heap_state(void)
{
    return s_heap_state;
}

// Internal setter — called only from platform/espidf/bb_net_health/bb_net_health_espidf.c.
// Not declared in the public header; the caller forward-declares it with extern.
void bb_board_set_heap_state(bb_board_heap_state_t state)
{
    s_heap_state = state;
}

const char *bb_board_heap_state_str(bb_board_heap_state_t state)
{
    switch (state) {
    case BB_BOARD_HEAP_STATE_OK:       return "ok";
    case BB_BOARD_HEAP_STATE_LOW:      return "low";
    case BB_BOARD_HEAP_STATE_CRITICAL: return "critical";
    default:                           return "ok";
    }
}
