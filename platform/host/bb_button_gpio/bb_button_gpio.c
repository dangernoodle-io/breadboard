// bb_button_gpio — host (native/test) driver implementation.
// Pin state is kept in memory; bb_button_host_inject_edge() simulates edges.
//
// Clock: BB_BUTTON_MOCK_CLOCK → callers supply now_ms to inject_edge directly;
//        ARDUINO → millis(); else clock_gettime CLOCK_MONOTONIC.
#include "bb_button_gpio.h"
#include "bb_button_gpio_host.h"
#include "bb_button_driver.h"
#include <stdlib.h>
#include <stddef.h>

#ifndef ARDUINO
#include <time.h>
#endif

// ---------------------------------------------------------------------------
// Clock (used by inject when caller doesn't supply timestamp — polling mode)
// ---------------------------------------------------------------------------

#ifndef BB_BUTTON_MOCK_CLOCK
#ifdef ARDUINO
static uint32_t now_ms(void) { return (uint32_t)millis(); }
#else
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif
#endif // !BB_BUTTON_MOCK_CLOCK

// ---------------------------------------------------------------------------
// Handle registry — maps handle → state_t* for inject_edge access
// ---------------------------------------------------------------------------

#define HOST_BTN_MAX 16

typedef struct {
    bb_button_handle_t handle;
    void *state;
} slot_t;

static slot_t s_slots[HOST_BTN_MAX];

static void slot_set(bb_button_handle_t h, void *st)
{
    for (int i = 0; i < HOST_BTN_MAX; i++) {
        if (!s_slots[i].handle) { s_slots[i].handle = h; s_slots[i].state = st; return; }
    }
}

static void *slot_get(bb_button_handle_t h)
{
    for (int i = 0; i < HOST_BTN_MAX; i++) {
        if (s_slots[i].handle == h) return s_slots[i].state;
    }
    return NULL;
}

static void slot_clear(bb_button_handle_t h)
{
    for (int i = 0; i < HOST_BTN_MAX; i++) {
        if (s_slots[i].handle == h) { s_slots[i].handle = NULL; s_slots[i].state = NULL; return; }
    }
}

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------

typedef struct {
    int gpio;
    bool active_low;
    bool raw_pressed;
    bb_button_handle_t handle;
    bb_button_driver_t drv; // per-instance vtable (owns debounce_ms cfg)
} state_t;

static bool op_is_pressed(void *st)
{
    return ((state_t *)st)->raw_pressed;
}

static bb_err_t op_poll(void *st)
{
    // Host driver is inject-driven; poll is a no-op.
    (void)st;
    return BB_OK;
}

static bb_err_t op_close(void *st)
{
    state_t *s = (state_t *)st;
    slot_clear(s->handle);
    free(s);
    return BB_OK;
}

bb_err_t bb_button_gpio_open(const bb_button_gpio_cfg_t *cfg, bb_button_handle_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0) return BB_ERR_INVALID_ARG;

    state_t *s = (state_t *)calloc(1, sizeof(state_t));
    if (!s) return BB_ERR_NO_SPACE;

    s->gpio        = cfg->gpio;
    s->active_low  = cfg->active_low;
    s->raw_pressed = false;

    // Per-instance vtable embedded in state so debounce_ms is cfg-driven.
    s->drv.is_pressed  = op_is_pressed;
    s->drv.poll        = op_poll;
    s->drv.close       = op_close;
    s->drv.debounce_ms = cfg->debounce_ms ? cfg->debounce_ms : 25;

    bb_err_t rc = bb_button_handle_create(&s->drv, s, out);
    if (rc != BB_OK) { free(s); return rc; }

    s->handle = *out;
    slot_set(*out, s);

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

void bb_button_host_inject_edge(bb_button_handle_t h, bool pressed, uint32_t inject_now_ms)
{
    if (!h) return;
    state_t *s = (state_t *)slot_get(h);
    if (!s) return;
    s->raw_pressed = pressed;
    bb_button_dispatch_raw(h, pressed, inject_now_ms);
}
