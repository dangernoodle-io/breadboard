// bb_button_driver.h — vtable interface for bb_button driver implementations.
// Consumers do NOT include this header; drivers depend on bb_button and include it.
#pragma once
#include "bb_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     (*is_pressed)(void *state);
    bb_err_t (*poll)      (void *state);
    bb_err_t (*close)     (void *state);
    uint16_t debounce_ms;
} bb_button_driver_t;

// Called from a driver's _open after allocating its state struct.
// On success *out holds the opaque public handle. The bb_button parent owns the
// wrapper allocation; bb_button_close frees it and calls drv->close(state).
// drv must point to a static-lifetime vtable.
bb_err_t bb_button_handle_create(const bb_button_driver_t *drv, void *state, bb_button_handle_t *out);

// Semi-private: called by drivers (from ISR-drain or poll) to report a raw
// pin level. Parent runs the debounce algorithm and emits events as needed.
bb_err_t bb_button_dispatch_raw(bb_button_handle_t h, bool raw_pressed, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
