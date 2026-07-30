// bb_log_flush()'s wait-loop decision core -- pure, portable (no platform
// dependency), host-testable. See bb_log_internal.h for the full rationale;
// the FreeRTOS-facing glue (platform/espidf/bb_log/bb_log.c) only drives
// xSemaphoreTake/xQueueSend/tick arithmetic around these functions.
#include "bb_log_internal.h"

bool bb_log_flush_seq_reached(uint32_t completed_seq, uint32_t target_seq)
{
    return (int32_t)(completed_seq - target_seq) >= 0;
}

bb_log_flush_wait_step_t bb_log_flush_wait_decide(bool take_succeeded,
                                                   uint32_t completed_seq,
                                                   uint32_t target_seq)
{
    if (!take_succeeded) {
        return BB_LOG_FLUSH_WAIT_TIMEOUT;
    }
    if (bb_log_flush_seq_reached(completed_seq, target_seq)) {
        return BB_LOG_FLUSH_WAIT_DONE;
    }
    return BB_LOG_FLUSH_WAIT_RETRY;
}

uint32_t bb_log_flush_remaining_ms(uint32_t budget_ms, uint32_t elapsed_ms)
{
    return (elapsed_ms < budget_ms) ? (budget_ms - elapsed_ms) : 0U;
}
