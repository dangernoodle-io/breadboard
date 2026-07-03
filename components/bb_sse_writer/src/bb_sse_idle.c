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

uint32_t bb_sse_abort_poll_slice_ms(uint32_t remaining_ms, uint32_t abort_poll_ms)
{
    if (remaining_ms == 0) {
        return 0;
    }
    return (remaining_ms < abort_poll_ms) ? remaining_ms : abort_poll_ms;
}
