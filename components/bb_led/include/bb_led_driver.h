// bb_led_driver.h — vtable interface for bb_led driver implementations.
// Consumers do NOT include this header; drivers depend on bb_led and include it.
#pragma once
#include "bb_led.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bb_err_t (*set_on)        (void *state, uint16_t idx, bool on);
    bb_err_t (*set_brightness)(void *state, uint16_t idx, uint8_t pct);
    bb_err_t (*set_color)     (void *state, uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
    bb_err_t (*flush)         (void *state);
    bb_err_t (*close)         (void *state);
    bb_led_caps_t caps;
    uint16_t count;
} bb_led_driver_t;

// Called from a driver's _open after allocating its state struct.
// On success *out holds the opaque public handle. The bb_led parent owns the
// wrapper allocation; bb_led_close frees it and calls drv->close(state).
// drv must point to a static-lifetime vtable. Vtable entries for absent caps
// must still be non-NULL; they should return BB_ERR_UNSUPPORTED defensively.
bb_err_t bb_led_handle_create(const bb_led_driver_t *drv, void *state, bb_led_handle_t *out);

#ifdef __cplusplus
}
#endif
