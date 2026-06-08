// ESP-IDF clock source for bb_fan autofan PID.
// Compiled only on ESP-IDF targets when CONFIG_BB_FAN_AUTOFAN=y.
// Provides bb_fan_autofan_inject_clock() to inject esp_timer into the PID.
// Consumers (e.g. asic_task) call this once after bb_fan_emc2101_open().
#ifdef CONFIG_BB_FAN_AUTOFAN

#include "bb_fan.h"
#include "esp_timer.h"

static unsigned long s_esp_now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

void bb_fan_autofan_inject_clock(bb_fan_handle_t h)
{
    bb_fan_autofan_set_clock(h, s_esp_now_ms);
}

#endif /* CONFIG_BB_FAN_AUTOFAN */
