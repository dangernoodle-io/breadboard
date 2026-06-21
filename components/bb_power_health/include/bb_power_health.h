// bb_power_health — pure vcore-collapse watchdog for ASIC power rails.
//
// No ESP-IDF or FreeRTOS dependencies; compiles on host and device.
//
// The watchdog evaluates power-rail health on each call and returns an
// action for the caller to perform.  All policy state is held by the caller
// (bb_vcore_wd_state_t); the module itself is stateless.
//
// Typical call site (periodic 1-second poll task):
//   bb_vcore_wd_input_t in = { .vcore_mv = measured_mv,
//                               .rail_enabled = true,
//                               .uptime_ms = bb_clock_now_ms() };
//   bb_vcore_wd_action_t act = bb_vcore_wd_eval(&s_wd, &in);
//   if (act == BB_VCORE_WD_RECOVER) bb_power_tps546_recover(h, &cfg);
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Thresholds — compile-time overridable via -D or Kconfig
// ---------------------------------------------------------------------------

// Warmup window: no action is taken during device boot.
#ifndef BB_VCORE_WD_WARMUP_MS
#define BB_VCORE_WD_WARMUP_MS      60000U
#endif

// A reading at or above this level is healthy.
#ifndef BB_VCORE_WD_OK_MV
#define BB_VCORE_WD_OK_MV          800
#endif

// A reading below this level (with rail enabled) is a collapse.
#ifndef BB_VCORE_WD_COLLAPSE_MV
#define BB_VCORE_WD_COLLAPSE_MV    500
#endif

// Number of consecutive collapsed readings before acting.
#ifndef BB_VCORE_WD_COLLAPSE_POLLS
#define BB_VCORE_WD_COLLAPSE_POLLS 3
#endif

// After this many ms of healthy readings the reboot-burst counter resets.
#ifndef BB_VCORE_WD_HEALTHY_RESET_MS
#define BB_VCORE_WD_HEALTHY_RESET_MS 300000U
#endif

// Maximum recover attempts within BB_VCORE_WD_WINDOW_MS before backing off.
#ifndef BB_VCORE_WD_BURST_MAX
#define BB_VCORE_WD_BURST_MAX      3
#endif

// Window for counting reboot bursts.
#ifndef BB_VCORE_WD_WINDOW_MS
#define BB_VCORE_WD_WINDOW_MS      600000U
#endif

// ---------------------------------------------------------------------------
// Action enum
// ---------------------------------------------------------------------------

/**
 * Action returned by bb_vcore_wd_eval() on each call.
 */
typedef enum {
    BB_VCORE_WD_NONE    = 0,  // no action needed
    BB_VCORE_WD_RECOVER = 1,  // caller should attempt a PMBus soft-recover
    BB_VCORE_WD_BACKOFF = 2,  // burst limit reached; skip this cycle but keep evaluating
} bb_vcore_wd_action_t;

// ---------------------------------------------------------------------------
// Input struct
// ---------------------------------------------------------------------------

/**
 * Snapshot of rail state fed to the watchdog each evaluation cycle.
 */
typedef struct {
    int      vcore_mv;      // measured core voltage in millivolts
    bool     rail_enabled;  // true when the regulator output is enabled
    uint64_t uptime_ms;     // monotonic uptime in milliseconds
} bb_vcore_wd_input_t;

// ---------------------------------------------------------------------------
// State struct (caller-owned, persistent across calls, zero-init valid)
// ---------------------------------------------------------------------------

/**
 * Persistent watchdog state.  Callers allocate this (typically as a static
 * module variable) and pass it on every bb_vcore_wd_eval() call.
 * Zero-init is a valid initial state.
 */
typedef struct {
    // Consecutive-low counter: how many back-to-back collapsed readings
    // since the last healthy reading.
    int      consec_low;

    // Reboot-burst tracking: number of RECOVER actions issued inside the
    // current burst window.
    int      burst_count;
    // Uptime timestamp when the current burst window started (0 = no burst).
    uint64_t burst_window_start_ms;

    // Healthy accumulator: continuous healthy time (ms) used to decide when
    // to reset the burst counter.
    uint64_t healthy_since_ms;  // uptime when the current healthy streak began (0 = not healthy)
    bool     in_healthy_streak; // true while vcore has been continuously >= BB_VCORE_WD_OK_MV
} bb_vcore_wd_state_t;

// ---------------------------------------------------------------------------
// Evaluator
// ---------------------------------------------------------------------------

/**
 * Evaluate vcore health and return the action the caller should perform.
 *
 * Logic:
 *   - Warmup (uptime < BB_VCORE_WD_WARMUP_MS): always BB_VCORE_WD_NONE.
 *   - Healthy (vcore >= BB_VCORE_WD_OK_MV): accumulate healthy time; after
 *     BB_VCORE_WD_HEALTHY_RESET_MS of continuous health, reset the burst
 *     counter; return BB_VCORE_WD_NONE and clear consec_low.
 *   - Collapsed (rail_enabled && vcore < BB_VCORE_WD_COLLAPSE_MV): bump
 *     consec_low; when it reaches BB_VCORE_WD_COLLAPSE_POLLS:
 *       - If burst_count < BB_VCORE_WD_BURST_MAX within BB_VCORE_WD_WINDOW_MS:
 *         return BB_VCORE_WD_RECOVER.
 *       - Else: return BB_VCORE_WD_BACKOFF (caller should skip recovery this
 *         tick but MUST keep calling eval so it recovers once rail is healthy).
 *   - Rail disabled: return BB_VCORE_WD_NONE (not our fault to recover).
 *
 * @param st  Persistent state (caller-owned; zero-init for first call).
 * @param in  Current rail snapshot.
 * @return Action to perform.
 */
bb_vcore_wd_action_t bb_vcore_wd_eval(bb_vcore_wd_state_t *st,
                                        const bb_vcore_wd_input_t *in);

#ifdef __cplusplus
}
#endif
