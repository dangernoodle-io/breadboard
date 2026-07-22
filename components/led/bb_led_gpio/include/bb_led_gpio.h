#pragma once
#include "bb_led.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gpio;
    bool active_low;  // true = drive LOW to turn on
} bb_led_gpio_cfg_t;

bb_err_t bb_led_gpio_open(const bb_led_gpio_cfg_t *cfg, bb_led_handle_t *out);

#ifdef __cplusplus
}
#endif
