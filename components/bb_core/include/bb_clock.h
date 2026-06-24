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

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#elif defined(ARDUINO)
// millis() is a global function supplied by the Arduino runtime; no header needed.
#else
#include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// u32 — for duration arithmetic where wrapping subtraction is correct.
static inline uint32_t bb_clock_now_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000u);
#elif defined(ARDUINO)
    return (uint32_t)millis();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

// u64 — for exposed absolute time points (no 49.7-day wrap).
static inline uint64_t bb_clock_now_ms64(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)(esp_timer_get_time() / 1000u);
#elif defined(ARDUINO)
    return (uint64_t)millis(); // u32 source — wraps at 49.7 days; see header note
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

#ifdef __cplusplus
}
#endif
