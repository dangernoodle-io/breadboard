#pragma once
#include "bb_core.h"
#include "bb_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gpio;
    bool active_low;       // default true — most dev boards use INPUT_PULLUP + active-low
    uint16_t debounce_ms;  // default 25
    bool prefer_isr;       // default true; falls back to polling automatically when not available
} bb_button_gpio_cfg_t;

bb_err_t bb_button_gpio_open(const bb_button_gpio_cfg_t *cfg, bb_button_handle_t *out);

#ifdef __cplusplus
}
#endif
