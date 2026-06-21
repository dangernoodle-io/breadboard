// bb_vcore_wd — pure vcore-collapse watchdog evaluator.
// No ESP-IDF / FreeRTOS dependencies; host-testable.
#include "bb_power_health.h"

bb_vcore_wd_action_t bb_vcore_wd_eval(bb_vcore_wd_state_t *st,
                                        const bb_vcore_wd_input_t *in)
{
    // OC-fault latch: if fault_held is set, refuse recovery until the
    // consumer explicitly calls bb_vcore_wd_clear_hold().  A healthy
    // reading does NOT auto-clear the latch — clearing is explicit only,
    // so the latch survives a power-cycle or auto-reboot (consumer seeds
    // fault_held from NVS on boot before the first eval).
    if (st->fault_held) {
        return BB_VCORE_WD_FAULT_HOLD;
    }

    // Warmup: suppress all actions during device boot.
    if (in->uptime_ms < BB_VCORE_WD_WARMUP_MS) {
        return BB_VCORE_WD_NONE;
    }

    // Healthy path: vcore is at or above the OK threshold.
    if (in->vcore_mv >= BB_VCORE_WD_OK_MV) {
        // Clear the consecutive-low counter — we're healthy.
        st->consec_low = 0;

        // Track continuous healthy time to decide when to reset burst counter.
        if (!st->in_healthy_streak) {
            st->in_healthy_streak = true;
            st->healthy_since_ms  = in->uptime_ms;
        } else {
            uint64_t healthy_duration = in->uptime_ms - st->healthy_since_ms;
            if (healthy_duration >= BB_VCORE_WD_HEALTHY_RESET_MS) {
                // Long enough healthy window: reset burst state.
                st->burst_count          = 0;
                st->burst_window_start_ms = 0;
                // Restart the streak so we don't re-trigger on the same window.
                st->healthy_since_ms = in->uptime_ms;
            }
        }
        return BB_VCORE_WD_NONE;
    }

    // Not healthy: clear the healthy streak.
    st->in_healthy_streak = false;
    st->healthy_since_ms  = 0;

    // Only act when the rail is enabled — if it's disabled the collapse is expected.
    if (!in->rail_enabled) {
        return BB_VCORE_WD_NONE;
    }

    // Collapsed path: rail enabled but vcore is below collapse threshold.
    if (in->vcore_mv < BB_VCORE_WD_COLLAPSE_MV) {
        st->consec_low++;

        if (st->consec_low < BB_VCORE_WD_COLLAPSE_POLLS) {
            // Not enough consecutive low readings yet; keep watching.
            return BB_VCORE_WD_NONE;
        }

        // Enough consecutive low readings — classify the collapse.

        // OC-fault: latch and hold.  Do NOT attempt recovery into a board
        // with an active over-current condition.
        if (in->oc_fault) {
            st->fault_held = true;
            return BB_VCORE_WD_FAULT_HOLD;
        }

        // Recoverable collapse (VIN sag, UNIT_OFF, etc.) — existing policy.

        // Check burst window: if the window has expired, reset the counter.
        if (st->burst_count > 0 &&
            (in->uptime_ms - st->burst_window_start_ms) > BB_VCORE_WD_WINDOW_MS) {
            st->burst_count           = 0;
            st->burst_window_start_ms = 0;
        }

        if (st->burst_count < BB_VCORE_WD_BURST_MAX) {
            // Open a new burst window on the first recovery attempt.
            if (st->burst_count == 0) {
                st->burst_window_start_ms = in->uptime_ms;
            }
            st->burst_count++;
            // Reset consec_low so we don't fire again immediately next tick.
            st->consec_low = 0;
            return BB_VCORE_WD_RECOVER;
        }

        // Burst limit reached: backoff this cycle but keep evaluating.
        // Do NOT reset consec_low so the caller stays in the backoff state
        // each tick until the rail recovers (goes healthy) or the burst window
        // expires (handled above on the next entry).
        return BB_VCORE_WD_BACKOFF;
    }

    // vcore is between COLLAPSE_MV and OK_MV — marginal but not collapsed.
    // Clear consec_low since we're not in a confirmed collapse.
    st->consec_low = 0;
    return BB_VCORE_WD_NONE;
}

bool bb_vcore_wd_is_held(const bb_vcore_wd_state_t *st)
{
    return st->fault_held;
}

void bb_vcore_wd_clear_hold(bb_vcore_wd_state_t *st)
{
    st->fault_held        = false;
    // Re-arm: clear transient counters so the next eval starts fresh.
    st->consec_low        = 0;
    st->in_healthy_streak = false;
    st->healthy_since_ms  = 0;
}
