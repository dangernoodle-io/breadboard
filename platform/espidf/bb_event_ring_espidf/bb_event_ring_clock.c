// ESP-IDF clock implementation for bb_event_ring.
// Provides bb_event_ring_now_us() backed by esp_timer_get_time().
// This file is compiled as part of the bb_event_ring_espidf component so that
// the portable bb_event_ring component needs no direct esp_timer dependency.
#include "esp_timer.h"
#include <stdint.h>

int64_t bb_event_ring_now_us(void)
{
    return esp_timer_get_time();
}
