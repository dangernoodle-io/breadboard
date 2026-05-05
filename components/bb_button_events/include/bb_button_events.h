// bb_button_events — semantic event layer on top of bb_button.
// Attach to a bb_button_handle_t, register a callback, and receive CLICK,
// DOUBLE_CLICK, LONG_PRESS_START, LONG_PRESS_END, and REPEAT events.
//
// The events layer is the button's single callback owner. Callers MUST NOT
// also call bb_button_set_callback on the same handle while attached.
// Detach does NOT close cfg.button — the caller retains ownership.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"
#include "bb_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_button_events *bb_button_events_handle_t;

typedef enum {
    BB_BTN_EVT_CLICK,
    BB_BTN_EVT_DOUBLE_CLICK,
    BB_BTN_EVT_LONG_PRESS_START,
    BB_BTN_EVT_LONG_PRESS_END,
    BB_BTN_EVT_REPEAT,
} bb_button_events_kind_t;

typedef struct {
    bb_button_events_kind_t kind;
    uint32_t timestamp_ms;
    uint32_t held_ms;   // for LONG_PRESS_END / REPEAT: total hold duration so far
} bb_button_events_event_t;

typedef void (*bb_button_events_cb_t)(const bb_button_events_event_t *e, void *user);

typedef struct {
    bb_button_handle_t   button;            // required; caller-owned
    uint16_t click_max_ms;                  // 0 → default 400; max press to count as click
    uint16_t double_click_window_ms;        // 0 → default 400; gap after release for 2nd click
    uint16_t long_press_ms;                 // 0 → default 800; hold threshold for long_press_start
    uint16_t repeat_interval_ms;            // 0 → default 100; period of REPEAT events while held
    uint16_t tick_period_ms;                // 0 → default 20 (50 Hz); ESP-IDF auto-timer period
    bool     auto_start_timer;              // ESP-IDF only; ignored on host/arduino
    bb_button_events_cb_t cb;
    void *user;
} bb_button_events_cfg_t;

// Attach the events layer to cfg->button and start processing. The events
// layer registers itself as the button's sole callback. Caller must not
// also set a callback on cfg->button while attached.
bb_err_t bb_button_events_attach(const bb_button_events_cfg_t *cfg,
                                 bb_button_events_handle_t *out);

// Advance the state machine by one tick. Always safe to call; no-op if the
// auto-timer already ran within tick_period_ms on ESP-IDF.
bb_err_t bb_button_events_tick  (bb_button_events_handle_t h);

// Stop timer (if any), unregister button callback, free handle.
// Does NOT close cfg->button.
bb_err_t bb_button_events_detach(bb_button_events_handle_t h);

#ifdef __cplusplus
}
#endif
