#pragma once

// bb_clock_now_ms() — portable monotonic millisecond counter.
//
// Platform routing:
//   ESP_PLATFORM  → esp_timer_get_time() / 1000  (wraps the same 64-bit us timer
//                   used by bb_timer_now_us(), avoiding a fourth time source)
//   ARDUINO       → millis()
//   host (default)→ clock_gettime(CLOCK_MONOTONIC)
//
// Components that need a settable mock for deterministic tests define their own
// per-component mock guard (e.g. BB_BUTTON_EVENTS_MOCK_CLOCK) and keep a local
// static; only the non-mock branch routes through bb_clock_now_ms().

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

#ifdef __cplusplus
}
#endif
