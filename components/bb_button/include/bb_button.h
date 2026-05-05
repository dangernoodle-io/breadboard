// bb_button — panel-agnostic button API (callback, poll, is_pressed, close).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_button *bb_button_handle_t;

typedef enum {
    BB_BTN_PRESSED,
    BB_BTN_RELEASED,
} bb_button_event_kind_t;

typedef struct {
    bb_button_event_kind_t kind;
    uint32_t timestamp_ms;
} bb_button_event_t;

typedef void (*bb_button_cb_t)(const bb_button_event_t *e, void *user);

bb_err_t bb_button_set_callback(bb_button_handle_t h, bb_button_cb_t cb, void *user);
bb_err_t bb_button_poll        (bb_button_handle_t h);   // safe to call always; no-op on full-ISR backends
bool     bb_button_is_pressed  (bb_button_handle_t h);   // current debounced state
bb_err_t bb_button_close       (bb_button_handle_t h);

// ESP-IDF only. Returns FreeRTOS QueueHandle_t (cast to void* to keep the
// public header free of FreeRTOS includes). NULL on non-IDF backends. Items
// pushed are bb_button_event_t-sized. Receive with xQueueReceive(...).
void *bb_button_get_queue(bb_button_handle_t h);

#ifdef __cplusplus
}
#endif
