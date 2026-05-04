// bb_led — panel-agnostic LED API (on/off, brightness, RGB, flush, close).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_led *bb_led_handle_t;

typedef enum {
    BB_LED_CAP_NONE       = 0,
    BB_LED_CAP_ONOFF      = 1u << 0,
    BB_LED_CAP_BRIGHTNESS = 1u << 1,
    BB_LED_CAP_RGB        = 1u << 2,
} bb_led_caps_t;

bb_led_caps_t bb_led_caps (bb_led_handle_t h);
uint16_t      bb_led_count(bb_led_handle_t h);

bb_err_t bb_led_set_on        (bb_led_handle_t h, uint16_t idx, bool on);
bb_err_t bb_led_set_brightness(bb_led_handle_t h, uint16_t idx, uint8_t pct); // 0..100
bb_err_t bb_led_set_color     (bb_led_handle_t h, uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
bb_err_t bb_led_fill_color    (bb_led_handle_t h, uint8_t r, uint8_t g, uint8_t b);
bb_err_t bb_led_flush         (bb_led_handle_t h);
bb_err_t bb_led_close         (bb_led_handle_t h);

#ifdef __cplusplus
}
#endif
