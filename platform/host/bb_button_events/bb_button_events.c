// bb_button_events — semantic event state machine (host/arduino platform).
// Auto-timer is not supported on host; consumers call bb_button_events_tick().
//
// When BB_BUTTON_EVENTS_MOCK_CLOCK is defined (test builds), now_ms() reads
// from a settable static; call bb_button_events_set_mock_time_ms() to advance.
#include "bb_button_events.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifndef ARDUINO
#include <time.h>
#endif

static const char *TAG = "bb_button_events";

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

#ifdef BB_BUTTON_EVENTS_MOCK_CLOCK
static uint32_t s_mock_time_ms = 0;
void bb_button_events_set_mock_time_ms(uint32_t ms) { s_mock_time_ms = ms; }
static uint32_t now_ms(void) { return s_mock_time_ms; }
#elif defined(ARDUINO)
static uint32_t now_ms(void) { return (uint32_t)millis(); }
#else
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

typedef enum {
    S_IDLE,
    S_PRESSED_PENDING,
    S_WAIT_DOUBLE,
    S_LONG_HELD,
} btn_evt_state_t;

// ---------------------------------------------------------------------------
// Handle struct
// ---------------------------------------------------------------------------

struct bb_button_events {
    bb_button_handle_t    button;
    bb_button_events_cb_t cb;
    void                 *user;

    uint16_t click_max_ms;
    uint16_t double_click_window_ms;
    uint16_t long_press_ms;
    uint16_t repeat_interval_ms;
    uint16_t tick_period_ms;

    btn_evt_state_t state;
    uint32_t        press_ms;
    uint32_t        release_ms;
    uint32_t        last_repeat_ms;
    uint32_t        last_tick_ms;
};

// ---------------------------------------------------------------------------
// Emit helper
// ---------------------------------------------------------------------------

static void emit(struct bb_button_events *h, bb_button_events_kind_t kind,
                 uint32_t timestamp_ms, uint32_t held_ms)
{
    if (!h->cb) return;
    bb_button_events_event_t e = {
        .kind         = kind,
        .timestamp_ms = timestamp_ms,
        .held_ms      = held_ms,
    };
    h->cb(&e, h->user);
}

// ---------------------------------------------------------------------------
// do_step: advance time-based transitions (called from tick and esp_timer cb)
// ---------------------------------------------------------------------------

static void do_step(struct bb_button_events *h)
{
    uint32_t now = now_ms();

    switch (h->state) {
    case S_IDLE:
        break;

    case S_PRESSED_PENDING:
        if (now - h->press_ms >= h->long_press_ms) {
            emit(h, BB_BTN_EVT_LONG_PRESS_START, now, 0);
            h->last_repeat_ms = now;
            h->state = S_LONG_HELD;
        }
        break;

    case S_WAIT_DOUBLE:
        if (now - h->release_ms > h->double_click_window_ms) {
            emit(h, BB_BTN_EVT_CLICK, now, 0);
            h->state = S_IDLE;
        }
        break;

    case S_LONG_HELD:
        if (now - h->last_repeat_ms >= h->repeat_interval_ms) {
            uint32_t held = now - h->press_ms;
            emit(h, BB_BTN_EVT_REPEAT, now, held);
            h->last_repeat_ms = now;
        }
        break;
    }

    h->last_tick_ms = now;
}

// ---------------------------------------------------------------------------
// Button callback — raw PRESSED/RELEASED from bb_button debouncer
// ---------------------------------------------------------------------------

static void btn_raw_cb(const bb_button_event_t *e, void *user)
{
    struct bb_button_events *h = (struct bb_button_events *)user;
    uint32_t ts = e->timestamp_ms;

    if (e->kind == BB_BTN_PRESSED) {
        switch (h->state) {
        case S_IDLE:
            h->press_ms = ts;
            h->state    = S_PRESSED_PENDING;
            break;

        case S_WAIT_DOUBLE:
            if (ts - h->release_ms <= h->double_click_window_ms) {
                emit(h, BB_BTN_EVT_DOUBLE_CLICK, ts, 0);
            }
            h->state = S_IDLE;
            break;

        default:
            // PRESSED while in PRESSED_PENDING or LONG_HELD — shouldn't happen
            // with a well-behaved button, but be safe.
            break;
        }
    } else { // BB_BTN_RELEASED
        switch (h->state) {
        case S_PRESSED_PENDING: {
            uint32_t held = ts - h->press_ms;
            if (held <= h->click_max_ms) {
                h->release_ms = ts;
                h->state      = S_WAIT_DOUBLE;
            } else {
                // medium press: outside click window, before long_press fired
                h->state = S_IDLE;
            }
            break;
        }

        case S_LONG_HELD: {
            uint32_t held = ts - h->press_ms;
            emit(h, BB_BTN_EVT_LONG_PRESS_END, ts, held);
            h->state = S_IDLE;
            break;
        }

        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_button_events_attach(const bb_button_events_cfg_t *cfg,
                                 bb_button_events_handle_t *out)
{
    if (!cfg || !out || !cfg->button) return BB_ERR_INVALID_ARG;

    struct bb_button_events *h =
        (struct bb_button_events *)calloc(1, sizeof *h);
    if (!h) return BB_ERR_NO_SPACE;

    h->button                = cfg->button;
    h->cb                    = cfg->cb;
    h->user                  = cfg->user;
    h->click_max_ms          = cfg->click_max_ms          ? cfg->click_max_ms          : 400u;
    h->double_click_window_ms = cfg->double_click_window_ms ? cfg->double_click_window_ms : 400u;
    h->long_press_ms         = cfg->long_press_ms         ? cfg->long_press_ms         : 800u;
    h->repeat_interval_ms    = cfg->repeat_interval_ms    ? cfg->repeat_interval_ms    : 100u;
    h->tick_period_ms        = cfg->tick_period_ms        ? cfg->tick_period_ms        : 20u;
    h->state                 = S_IDLE;
    h->last_tick_ms          = now_ms();

    bb_button_set_callback(cfg->button, btn_raw_cb, h);

    *out = h;
    bb_log_i(TAG, "attached (tick=%"PRIu16"ms)", h->tick_period_ms);
    return BB_OK;
}

bb_err_t bb_button_events_tick(bb_button_events_handle_t h)
{
    if (!h) return BB_ERR_INVALID_ARG;
    uint32_t now = now_ms();
    if (now - h->last_tick_ms < h->tick_period_ms) return BB_OK;
    do_step(h);
    return BB_OK;
}

bb_err_t bb_button_events_detach(bb_button_events_handle_t h)
{
    if (!h) return BB_ERR_INVALID_ARG;
    bb_button_set_callback(h->button, NULL, NULL);
    free(h);
    return BB_OK;
}
