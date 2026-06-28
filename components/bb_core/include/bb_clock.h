#pragma once

// Portable monotonic millisecond helpers.
//
// bb_clock_now_ms()   — uint32_t, wraps at 49.7 days.  Use for u32 duration
//                       arithmetic (e.g. last_publish_ms, last_sample_ms) where
//                       wrapping subtraction is intentional and correct.
//
// bb_clock_now_ms64() — uint64_t, no wrap.  Use for exposed absolute time points
//                       (JSON fields, public status structs) where a 49.7-day wrap
//                       would be a contract violation.  Also use when assigning to
//                       a uint64_t *_ms field — never feed a u32 bb_clock_now_ms()
//                       into a u64 field (the truncation is silent and wrong at
//                       wrap boundaries).
//
// Platform routing (both helpers):
//   ESP_PLATFORM  → esp_timer_get_time() / 1000  (64-bit µs timer; u32 truncates
//                   the low 32 bits, giving correct u32 subtraction semantics)
//   ARDUINO       → millis() (u32 source; bb_clock_now_ms64 casts to u64 — correct
//                   for durations up to 49.7 days; callers needing longer spans
//                   must maintain their own epoch-wrap logic)
//   host (default)→ clock_gettime(CLOCK_MONOTONIC)
//
// Components that need a settable mock for deterministic tests define their own
// per-component mock guard (e.g. BB_BUTTON_EVENTS_MOCK_CLOCK) and keep a local
// static; only the non-mock branch routes through these helpers.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// u32 — for duration arithmetic where wrapping subtraction is correct.
uint32_t bb_clock_now_ms(void);

// u64 — for exposed absolute time points (no 49.7-day wrap).
uint64_t bb_clock_now_ms64(void);

#ifdef __cplusplus
}
#endif
