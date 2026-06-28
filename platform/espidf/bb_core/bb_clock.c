// bb_clock — ESP-IDF implementation using esp_timer_get_time().
// Compiled on ESP-IDF targets only; host builds use platform/host/bb_core/bb_clock.c.

#include "bb_clock.h"
#include "esp_timer.h"

uint64_t bb_clock_now_ms64(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000u);
}

uint32_t bb_clock_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000u);
}
