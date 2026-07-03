#include "sse_pool_reclaim_decision.h"

sse_pool_reclaim_action_t sse_pool_reclaim_decide(size_t active_clients,
                                                  size_t acquired_slots,
                                                  size_t pending_corpses)
{
    if (active_clients == 0 && acquired_slots == 0 && pending_corpses == 0) {
        return SSE_POOL_RECLAIM_DESTROY;
    }
    return SSE_POOL_RECLAIM_KEEP;
}
