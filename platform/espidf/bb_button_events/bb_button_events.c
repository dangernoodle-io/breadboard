// bb_button_events — semantic event state machine (ESP-IDF platform).
// Kept in sync with platform/host/bb_button_events/bb_button_events.c; differs
// only in the now_ms() implementation and the optional esp_timer auto-start path.
#include "bb_button_events.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

static const char *TAG = "bb_button_events";

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

static uint32_t now_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000LL);
#else
    return 0;
#endif
}

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

#ifdef ESP_PLATFORM
    esp_timer_handle_t esp_timer;
#endif
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
// do_step: advance time-based transitions
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

#ifdef ESP_PLATFORM
static void esp_timer_cb(void *arg)
{
    struct bb_button_events *h = (struct bb_button_events *)arg;
    do_step(h);
}
#endif

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

    h->button                 = cfg->button;
    h->cb                     = cfg->cb;
    h->user                   = cfg->user;
    h->click_max_ms           = cfg->click_max_ms          ? cfg->click_max_ms          : 400u;
    h->double_click_window_ms = cfg->double_click_window_ms ? cfg->double_click_window_ms : 400u;
    h->long_press_ms          = cfg->long_press_ms         ? cfg->long_press_ms         : 800u;
    h->repeat_interval_ms     = cfg->repeat_interval_ms    ? cfg->repeat_interval_ms    : 100u;
    h->tick_period_ms         = cfg->tick_period_ms        ? cfg->tick_period_ms        : 20u;
    h->state                  = S_IDLE;
    h->last_tick_ms           = now_ms();

#ifdef ESP_PLATFORM
    h->esp_timer = NULL;
    if (cfg->auto_start_timer) {
        const esp_timer_create_args_t args = {
            .callback        = esp_timer_cb,
            .arg             = h,
            .name            = "bb_btn_evt",
            .dispatch_method = ESP_TIMER_TASK,
        };
        esp_err_t rc = esp_timer_create(&args, &h->esp_timer);
        if (rc != ESP_OK) {
            free(h);
            return BB_ERR_INVALID_STATE;
        }
        esp_timer_start_periodic(h->esp_timer, (uint64_t)h->tick_period_ms * 1000u);
    }
#endif

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
#ifdef ESP_PLATFORM
    if (h->esp_timer) {
        esp_timer_stop(h->esp_timer);
        esp_timer_delete(h->esp_timer);
        h->esp_timer = NULL;
    }
#endif
    bb_button_set_callback(h->button, NULL, NULL);
    free(h);
    return BB_OK;
}
