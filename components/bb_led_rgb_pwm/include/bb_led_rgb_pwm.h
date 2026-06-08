#pragma once
#include "bb_led.h"
#ifdef __cplusplus
extern "C" {
#endif
// 3-GPIO RGB LED on three LEDC channels (LEDC_TIMER_0 LOW_SPEED). Single
// active_low sense — common-anode RGB ties all cathodes active-low together.
typedef struct {
    int gpio_r, gpio_g, gpio_b;
    uint32_t freq_hz;
    uint8_t resolution_bits;   // 1..14
    bool active_low;
} bb_led_rgb_pwm_cfg_t;
bb_err_t bb_led_rgb_pwm_open(const bb_led_rgb_pwm_cfg_t *cfg, bb_led_handle_t *out);
#ifdef __cplusplus
}
#endif
