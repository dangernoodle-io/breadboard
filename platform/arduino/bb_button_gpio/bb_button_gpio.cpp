// bb_button_gpio — Arduino driver.
// ISR mode: attachInterrupt(CHANGE) where available; edge count drives poll().
// Polling mode: pure digitalRead in poll().
//
// Up to 4 instances supported in ISR mode (Arduino ISRs can't take arguments;
// per-instance thunks are resolved from a static array). If a 5th instance
// requests ISR and all slots are full, it falls back to polling.
//
// cb dispatch context: poll() (called from loop()). Never ISR context.
extern "C" {
#include "bb_button_gpio.h"
#include "bb_button_driver.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
}
#include <Arduino.h>

// NOT_AN_INTERRUPT is -1 on most Arduino cores; some platforms (Renesas R4)
// omit it and expose IS_PIN_INTERRUPT() instead. Provide a fallback so the
// driver compiles universally; the runtime comparison is equivalent.
#ifndef NOT_AN_INTERRUPT
#define NOT_AN_INTERRUPT (-1)
#endif

extern "C" {

// ---------------------------------------------------------------------------
// ISR thunk table (up to 4 instances)
// ---------------------------------------------------------------------------

#define BB_BUTTON_GPIO_MAX_ISR 4

struct isr_slot_t {
    volatile uint8_t edge_count;
    void *state_ptr; // state_t*
    bool active;
};

static isr_slot_t s_isr_slots[BB_BUTTON_GPIO_MAX_ISR];

// One thunk per slot — cannot use lambdas with attachInterrupt on all toolchains.
static void thunk0(void) { if (s_isr_slots[0].active) s_isr_slots[0].edge_count++; }
static void thunk1(void) { if (s_isr_slots[1].active) s_isr_slots[1].edge_count++; }
static void thunk2(void) { if (s_isr_slots[2].active) s_isr_slots[2].edge_count++; }
static void thunk3(void) { if (s_isr_slots[3].active) s_isr_slots[3].edge_count++; }

typedef void (*thunk_fn)(void);
static const thunk_fn s_thunks[BB_BUTTON_GPIO_MAX_ISR] = { thunk0, thunk1, thunk2, thunk3 };

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------

typedef struct {
    int gpio;
    bool active_low;
    int isr_slot;       // -1 if polling-only
    bb_button_handle_t handle;
    bb_button_driver_t drv;
} state_t;

static bool op_is_pressed(void *st)
{
    state_t *s = (state_t *)st;
    int level = digitalRead(s->gpio);
    return s->active_low ? (level == LOW) : (level == HIGH);
}

static bb_err_t op_poll(void *st)
{
    state_t *s = (state_t *)st;
    uint32_t now = (uint32_t)millis();

    if (s->isr_slot >= 0) {
        // ISR mode: check edge_count (atomic 8-bit read on AVR).
        // Even if edge_count is 0 we still read the pin in case we missed an edge.
        uint8_t ec;
#if defined(__AVR__)
        // AVR: 8-bit read is inherently atomic; no cli/sei needed for the read.
        ec = s_isr_slots[s->isr_slot].edge_count;
        if (ec) s_isr_slots[s->isr_slot].edge_count = 0;
#else
        ec = s_isr_slots[s->isr_slot].edge_count;
        if (ec) s_isr_slots[s->isr_slot].edge_count = 0;
#endif
        // Read actual pin level regardless (catches missed ISRs).
        int level = digitalRead(s->gpio);
        bool raw = s->active_low ? (level == LOW) : (level == HIGH);
        if (ec || true) {
            bb_button_dispatch_raw(s->handle, raw, now);
        }
    } else {
        // Polling mode.
        int level = digitalRead(s->gpio);
        bool raw = s->active_low ? (level == LOW) : (level == HIGH);
        bb_button_dispatch_raw(s->handle, raw, now);
    }
    return BB_OK;
}

static bb_err_t op_close(void *st)
{
    state_t *s = (state_t *)st;
    if (s->isr_slot >= 0) {
        detachInterrupt(digitalPinToInterrupt(s->gpio));
        s_isr_slots[s->isr_slot].active   = false;
        s_isr_slots[s->isr_slot].state_ptr = NULL;
    }
    pinMode(s->gpio, INPUT);
    free(s);
    return BB_OK;
}

bb_err_t bb_button_gpio_open(const bb_button_gpio_cfg_t *cfg, bb_button_handle_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0) return BB_ERR_INVALID_ARG;

    state_t *s = (state_t *)calloc(1, sizeof(state_t));
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio       = cfg->gpio;
    s->active_low = cfg->active_low;
    s->isr_slot   = -1;

    s->drv.is_pressed  = op_is_pressed;
    s->drv.poll        = op_poll;
    s->drv.close       = op_close;
    s->drv.debounce_ms = cfg->debounce_ms ? cfg->debounce_ms : 25;

    // Configure pin.
    pinMode(s->gpio, cfg->active_low ? INPUT_PULLUP : INPUT);

    // Try ISR mode.
    if (cfg->prefer_isr) {
        int intnum = digitalPinToInterrupt(s->gpio);
        if (intnum != NOT_AN_INTERRUPT) {
            // Find a free ISR slot.
            for (int i = 0; i < BB_BUTTON_GPIO_MAX_ISR; i++) {
                if (!s_isr_slots[i].active) {
                    s_isr_slots[i].active     = true;
                    s_isr_slots[i].edge_count = 0;
                    s_isr_slots[i].state_ptr  = s;
                    s->isr_slot = i;
                    attachInterrupt(intnum, s_thunks[i], CHANGE);
                    break;
                }
            }
            // If no slot available, fall through to polling mode (isr_slot stays -1).
        }
    }

    bb_err_t rc = bb_button_handle_create(&s->drv, s, out);
    if (rc != BB_OK) {
        if (s->isr_slot >= 0) {
            detachInterrupt(digitalPinToInterrupt(s->gpio));
            s_isr_slots[s->isr_slot].active = false;
            s_isr_slots[s->isr_slot].state_ptr = NULL;
        }
        free(s);
        return rc;
    }
    s->handle = *out;

    return BB_OK;
}

} // extern "C"
