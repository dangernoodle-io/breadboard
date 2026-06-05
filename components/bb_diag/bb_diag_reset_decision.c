#include "bb_diag.h"

bb_diag_reset_result_t bb_diag_reset_decision(uint32_t stored_fp, uint32_t running_fp,
                                               uint32_t stored_count, bool is_abnormal)
{
    bb_diag_reset_result_t result;

    if (stored_fp == 0 || stored_fp != running_fp) {
        /* New firmware (or first boot): reset to clean baseline; do not count
         * the deploy boot as an abnormal reset. */
        result.new_count = 0;
        result.store_fp  = true;
    } else {
        /* Same firmware as last recorded: maintain the running count. */
        result.new_count = stored_count + (is_abnormal ? 1u : 0u);
        result.store_fp  = false;
    }

    return result;
}
