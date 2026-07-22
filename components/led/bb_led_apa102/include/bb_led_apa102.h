#pragma once
#include "bb_led.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int pin_clk;
    int pin_din;
    uint16_t led_count;
    uint8_t global_brightness_31;  // 0..31 — APA102 5-bit per-LED dim
} bb_led_apa102_cfg_t;

bb_err_t bb_led_apa102_open(const bb_led_apa102_cfg_t *cfg, bb_led_handle_t *out);

#ifdef __cplusplus
}
#endif
