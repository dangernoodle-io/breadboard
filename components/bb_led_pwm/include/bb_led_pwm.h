#pragma once
#include "bb_led.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gpio;
    uint32_t freq_hz;          // PWM frequency, e.g. 5000
    uint8_t resolution_bits;   // 1..14 on ESP-IDF LEDC; ignored on Arduino (fixed at 8)
    bool active_low;           // true = pct 0 → duty max, pct 100 → duty 0
} bb_led_pwm_cfg_t;

bb_err_t bb_led_pwm_open(const bb_led_pwm_cfg_t *cfg, bb_led_handle_t *out);

#ifdef __cplusplus
}
#endif
