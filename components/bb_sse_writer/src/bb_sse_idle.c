#include "bb_sse_idle.h"

uint32_t bb_sse_idle_advance(uint32_t accumulated, uint32_t step_ms,
                             uint32_t heartbeat_ms, bool *should_ping)
{
    accumulated += step_ms;
    if (accumulated >= heartbeat_ms) {
        *should_ping = true;
        return 0;
    }
    *should_ping = false;
    return accumulated;
}
