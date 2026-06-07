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

// Returns the driver's static name string (e.g. "apa102", "pwm", "gpio"),
// or NULL if h is invalid or the driver has no name.
const char   *bb_led_name (bb_led_handle_t h);

// Records h as the app's designated primary/status LED handle for introspection.
// bb_led itself stays multi-handle/handle-based; this is a single app-level slot.
// Pass NULL to clear. Does not transfer ownership.
void          bb_led_set_primary(bb_led_handle_t h);

// Returns the handle recorded by bb_led_set_primary(), or NULL if none set.
bb_led_handle_t bb_led_primary(void);

bb_err_t bb_led_set_on        (bb_led_handle_t h, uint16_t idx, bool on);
bb_err_t bb_led_set_brightness(bb_led_handle_t h, uint16_t idx, uint8_t pct); // 0..100
// Fine-resolution brightness: level 0..65535, perceptual (driver applies gamma).
// Requires BB_LED_CAP_BRIGHTNESS; bridges to set_brightness on drivers without it.
bb_err_t bb_led_set_level     (bb_led_handle_t h, uint16_t idx, uint16_t level);
bb_err_t bb_led_set_color     (bb_led_handle_t h, uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
bb_err_t bb_led_fill_color    (bb_led_handle_t h, uint8_t r, uint8_t g, uint8_t b);
bb_err_t bb_led_flush         (bb_led_handle_t h);
bb_err_t bb_led_close         (bb_led_handle_t h);

#ifdef __cplusplus
}
#endif
