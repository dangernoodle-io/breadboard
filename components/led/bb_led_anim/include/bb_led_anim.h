// bb_led_anim — pattern/animation library on top of bb_led handles.
// Consumers attach an animator to a bb_led handle, set a pattern, and either
// let a timer drive ticks (ESP-IDF, auto_start_timer=true) or call
// bb_led_anim_tick() from loop() (Arduino) or a test (host).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"
#include "bb_led.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_led_anim *bb_led_anim_handle_t;

typedef enum {
    BB_ANIM_SOLID,
    BB_ANIM_BLINK,
    BB_ANIM_BREATHE,
    BB_ANIM_PULSE,
    BB_ANIM_COLOR_CYCLE,
    BB_ANIM_CHASE,
} bb_led_anim_kind_t;

typedef struct {
    bb_led_anim_kind_t kind;
    union {
        struct { uint8_t r, g, b, brightness_pct; } solid;
        // level_pct 0 → legacy full on/off (BB_LED_CAP_ONOFF); >0 → flash at that
        // brightness via set_level (gamma-correct, needs BB_LED_CAP_BRIGHTNESS).
        struct { uint32_t period_ms; uint8_t duty_pct; uint8_t level_pct; } blink;
        struct { uint32_t period_ms; uint8_t min_pct, max_pct; } breathe;
        struct { uint32_t period_ms; uint8_t peak_pct; uint32_t decay_ms; } pulse;
        struct { uint32_t period_ms; uint8_t sat_pct, val_pct; } color_cycle;
        struct { uint32_t period_ms; uint8_t r, g, b; uint8_t tail_len; } chase;
    };
} bb_led_anim_pattern_t;

// Defaults applied when the corresponding pattern field is 0.
#define BB_LED_ANIM_BLINK_PERIOD_DEFAULT_MS        1000
#define BB_LED_ANIM_BREATHE_PERIOD_DEFAULT_MS      2000
#define BB_LED_ANIM_PULSE_PERIOD_DEFAULT_MS        1000
#define BB_LED_ANIM_COLOR_CYCLE_PERIOD_DEFAULT_MS  3000
#define BB_LED_ANIM_CHASE_PERIOD_DEFAULT_MS        1000
#define BB_LED_ANIM_TICK_DEFAULT_MS                  20

typedef struct {
    bb_led_handle_t led;
    uint32_t tick_period_ms;  // 0 → default BB_LED_ANIM_TICK_DEFAULT_MS (50 Hz)
    bool auto_start_timer;    // ESP-IDF only; ignored on host/arduino
} bb_led_anim_cfg_t;

// Allocate and attach an animator to cfg->led. Caller retains ownership of the
// led handle; bb_led_anim_detach does NOT close it.
bb_err_t bb_led_anim_attach(const bb_led_anim_cfg_t *cfg, bb_led_anim_handle_t *out);

// Swap the active pattern. Resets phase to now. Checks caps; returns
// BB_ERR_UNSUPPORTED when the handle's bb_led lacks the required caps.
bb_err_t bb_led_anim_set   (bb_led_anim_handle_t h, const bb_led_anim_pattern_t *pat);

// Like bb_led_anim_set, but cross-fades from the current output level into the
// new pattern over fade_ms (brightness/level patterns only — solid, breathe,
// pulse, blink-with-level; RGB colour is not crossfaded). fade_ms 0 is identical
// to bb_led_anim_set. The fade runs in perceptual set_level space.
bb_err_t bb_led_anim_set_transition(bb_led_anim_handle_t h,
                                    const bb_led_anim_pattern_t *pat, uint32_t fade_ms);

// Suspend/resume ticking (pattern state is preserved).
bb_err_t bb_led_anim_pause (bb_led_anim_handle_t h);
bb_err_t bb_led_anim_resume(bb_led_anim_handle_t h);

// Advance the animation by one tick. On host/Arduino the caller drives this.
// On ESP-IDF with auto_start_timer=true the timer callback calls this internally;
// the consumer may still call it but it will be a no-op if called within
// tick_period_ms of the last tick.
bb_err_t bb_led_anim_tick  (bb_led_anim_handle_t h);

// Stop timer (if any), free handle. Does NOT close the underlying bb_led handle.
bb_err_t bb_led_anim_detach(bb_led_anim_handle_t h);

#ifdef __cplusplus
}
#endif
