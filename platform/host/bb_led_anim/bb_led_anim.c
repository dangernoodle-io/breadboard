// bb_led_anim — pattern/animation library (host/arduino platform).
// Auto-timer is not supported on host; consumers call bb_led_anim_tick().
//
// When BB_LED_ANIM_MOCK_CLOCK is defined (test builds), now_ms() reads from a
// settable static; call bb_led_anim_set_mock_time_ms() to advance time.
#include "bb_led_anim.h"
#include "bb_led.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifndef ARDUINO
#include <time.h>
#endif

static const char *TAG = "bb_led_anim";

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

#ifdef BB_LED_ANIM_MOCK_CLOCK
static uint32_t s_mock_time_ms = 0;
void bb_led_anim_set_mock_time_ms(uint32_t ms) { s_mock_time_ms = ms; }
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
// Sin LUT: sin8(i) = 128 + 127*sin(2*pi*i/256), values [0..255].
// ---------------------------------------------------------------------------

static const uint8_t s_sin8[256] = {
    128,131,134,137,140,143,146,149,152,155,158,162,165,167,170,173,
    176,179,182,185,188,190,193,196,198,201,203,206,208,211,213,215,
    218,220,222,224,226,228,230,232,234,235,237,238,240,241,243,244,
    245,246,247,248,249,250,250,251,252,252,253,253,253,254,254,254,
    254,254,254,254,253,253,253,252,252,251,250,250,249,248,247,246,
    245,244,243,241,240,238,237,235,234,232,230,228,226,224,222,220,
    218,215,213,211,208,206,203,201,198,196,193,190,188,185,182,179,
    176,173,170,167,165,162,158,155,152,149,146,143,140,137,134,131,
    128,124,121,118,115,112,109,106,103,100, 97, 93, 90, 88, 85, 82,
     79, 76, 73, 70, 67, 65, 62, 59, 57, 54, 52, 49, 47, 44, 42, 40,
     37, 35, 33, 31, 29, 27, 25, 23, 21, 20, 18, 17, 15, 14, 12, 11,
     10,  9,  8,  7,  6,  5,  5,  4,  3,  3,  2,  2,  2,  1,  1,  1,
      1,  1,  1,  1,  2,  2,  2,  3,  3,  4,  5,  5,  6,  7,  8,  9,
     10, 11, 12, 14, 15, 17, 18, 20, 21, 23, 25, 27, 29, 31, 33, 35,
     37, 40, 42, 44, 47, 49, 52, 54, 57, 59, 62, 65, 67, 70, 73, 76,
     79, 82, 85, 88, 90, 93, 97,100,103,106,109,112,115,118,121,124,
};

static inline uint8_t sin8(uint8_t phase) { return s_sin8[phase]; }

// ---------------------------------------------------------------------------
// Integer HSV → RGB (Wikipedia integer algorithm).
// h: 0..359, s: 0..255, v: 0..255.
// ---------------------------------------------------------------------------

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) { *r = *g = *b = v; return; }
    uint16_t hi = (h / 60u) % 6u;
    uint32_t f  = (h % 60u) * 255u / 60u;
    uint8_t  p  = (uint8_t)((uint32_t)v * (255u - s) / 255u);
    uint8_t  q  = (uint8_t)((uint32_t)v * (255u - f * s / 255u) / 255u);
    uint8_t  t  = (uint8_t)((uint32_t)v * (255u - (255u - f) * s / 255u) / 255u);
    switch (hi) {
    case 0: *r=v; *g=t; *b=p; break;
    case 1: *r=q; *g=v; *b=p; break;
    case 2: *r=p; *g=v; *b=t; break;
    case 3: *r=p; *g=q; *b=v; break;
    case 4: *r=t; *g=p; *b=v; break;
    default:*r=v; *g=p; *b=q; break;
    }
}

// ---------------------------------------------------------------------------
// Handle struct
// ---------------------------------------------------------------------------

struct bb_led_anim {
    bb_led_handle_t       led;
    bb_led_anim_pattern_t pat;
    uint32_t              tick_period_ms;
    uint32_t              start_ms;
    uint32_t              last_tick_ms;
    bool                  paused;
    bool                  has_pattern;
    bool                  dirty;  // for SOLID: write-once flag
};

// ---------------------------------------------------------------------------
// Pattern step functions
// ---------------------------------------------------------------------------

static void step_solid(struct bb_led_anim *h)
{
    if (!h->dirty) return;
    uint16_t cnt      = bb_led_count(h->led);
    bb_led_caps_t caps = bb_led_caps(h->led);
    if (caps & BB_LED_CAP_RGB)
        bb_led_fill_color(h->led, h->pat.solid.r, h->pat.solid.g, h->pat.solid.b);
    if (caps & BB_LED_CAP_BRIGHTNESS)
        for (uint16_t i = 0; i < cnt; i++)
            bb_led_set_brightness(h->led, i, h->pat.solid.brightness_pct);
    if (caps & BB_LED_CAP_ONOFF)
        for (uint16_t i = 0; i < cnt; i++)
            bb_led_set_on(h->led, i, true);
    bb_led_flush(h->led);
    h->dirty = false;
}

static void step_blink(struct bb_led_anim *h, uint32_t elapsed_ms)
{
    uint32_t period = h->pat.blink.period_ms ? h->pat.blink.period_ms : 1000u;
    uint32_t phase  = elapsed_ms % period;
    bool on = phase < (uint32_t)period * h->pat.blink.duty_pct / 100u;
    uint16_t cnt = bb_led_count(h->led);
    for (uint16_t i = 0; i < cnt; i++)
        bb_led_set_on(h->led, i, on);
    bb_led_flush(h->led);
}

static void step_breathe(struct bb_led_anim *h, uint32_t elapsed_ms)
{
    uint32_t period   = h->pat.breathe.period_ms ? h->pat.breathe.period_ms : 2000u;
    uint32_t phase_ms = elapsed_ms % period;
    uint8_t  sin_idx  = (uint8_t)((uint32_t)phase_ms * 256u / period);
    uint8_t  s        = sin8(sin_idx);
    uint8_t  min_pct  = h->pat.breathe.min_pct;
    uint8_t  max_pct  = h->pat.breathe.max_pct;
    uint8_t  range    = (max_pct > min_pct) ? (max_pct - min_pct) : 0u;
    uint8_t  pct      = min_pct + (uint8_t)((uint32_t)s * range / 255u);
    uint16_t cnt = bb_led_count(h->led);
    for (uint16_t i = 0; i < cnt; i++)
        bb_led_set_brightness(h->led, i, pct);
    bb_led_flush(h->led);
}

static void step_pulse(struct bb_led_anim *h, uint32_t elapsed_ms)
{
    uint32_t period   = h->pat.pulse.period_ms ? h->pat.pulse.period_ms : 1000u;
    uint32_t decay_ms = h->pat.pulse.decay_ms  ? h->pat.pulse.decay_ms  : period / 2u;
    uint32_t phase    = elapsed_ms % period;
    uint32_t ramp_ms  = (period > decay_ms) ? (period - decay_ms) / 2u : 1u;
    uint8_t  peak     = h->pat.pulse.peak_pct;
    uint8_t  pct;

    if (phase < ramp_ms) {
        pct = (uint8_t)((uint32_t)phase * peak / ramp_ms);
    } else if (phase < ramp_ms + decay_ms) {
        uint32_t d         = phase - ramp_ms;
        uint32_t remaining = (uint32_t)peak * (decay_ms - d) / decay_ms;
        pct = (uint8_t)(remaining > 100u ? 100u : remaining);
    } else {
        pct = 0;
    }
    uint16_t cnt = bb_led_count(h->led);
    for (uint16_t i = 0; i < cnt; i++)
        bb_led_set_brightness(h->led, i, pct);
    bb_led_flush(h->led);
}

static void step_color_cycle(struct bb_led_anim *h, uint32_t elapsed_ms)
{
    uint32_t period = h->pat.color_cycle.period_ms ? h->pat.color_cycle.period_ms : 3000u;
    uint16_t hue    = (uint16_t)((elapsed_ms % period) * 360u / period);
    uint8_t  sat    = (uint8_t)((uint32_t)h->pat.color_cycle.sat_pct * 255u / 100u);
    uint8_t  val    = (uint8_t)((uint32_t)h->pat.color_cycle.val_pct * 255u / 100u);
    uint8_t  r, g, b;
    hsv_to_rgb(hue, sat, val, &r, &g, &b);
    bb_led_fill_color(h->led, r, g, b);
    bb_led_flush(h->led);
}

static void step_chase(struct bb_led_anim *h, uint32_t elapsed_ms)
{
    uint16_t cnt    = bb_led_count(h->led);
    uint32_t period = h->pat.chase.period_ms ? h->pat.chase.period_ms : 1000u;
    uint16_t head   = (uint16_t)((elapsed_ms % period) * cnt / period);
    uint8_t  tail   = h->pat.chase.tail_len ? h->pat.chase.tail_len : 3u;

    for (uint16_t i = 0; i < cnt; i++) {
        uint16_t dist = (uint16_t)((cnt + head - i) % cnt);
        if (dist == 0) {
            bb_led_set_color(h->led, i, h->pat.chase.r, h->pat.chase.g, h->pat.chase.b);
        } else if (dist <= tail) {
            uint8_t fade = (uint8_t)((uint32_t)(tail - dist + 1u) * 255u / (tail + 1u));
            bb_led_set_color(h->led, i,
                (uint8_t)((uint32_t)h->pat.chase.r * fade / 255u),
                (uint8_t)((uint32_t)h->pat.chase.g * fade / 255u),
                (uint8_t)((uint32_t)h->pat.chase.b * fade / 255u));
        } else {
            bb_led_set_color(h->led, i, 0, 0, 0);
        }
    }
    bb_led_flush(h->led);
}

static void step(struct bb_led_anim *h)
{
    if (h->paused || !h->has_pattern) return;
    uint32_t n       = now_ms();
    uint32_t elapsed = n - h->start_ms;

    switch (h->pat.kind) {
    case BB_ANIM_SOLID:       step_solid(h);               break;
    case BB_ANIM_BLINK:       step_blink(h, elapsed);      break;
    case BB_ANIM_BREATHE:     step_breathe(h, elapsed);    break;
    case BB_ANIM_PULSE:       step_pulse(h, elapsed);      break;
    case BB_ANIM_COLOR_CYCLE: step_color_cycle(h, elapsed); break;
    case BB_ANIM_CHASE:       step_chase(h, elapsed);      break;
    }
    h->last_tick_ms = n;
}

// ---------------------------------------------------------------------------
// Caps helpers
// ---------------------------------------------------------------------------

static bb_led_caps_t pattern_required_caps(const bb_led_anim_pattern_t *pat,
                                            uint16_t led_count)
{
    switch (pat->kind) {
    case BB_ANIM_SOLID:       return BB_LED_CAP_ONOFF;
    case BB_ANIM_BLINK:       return BB_LED_CAP_ONOFF;
    case BB_ANIM_BREATHE:     return BB_LED_CAP_BRIGHTNESS;
    case BB_ANIM_PULSE:       return BB_LED_CAP_BRIGHTNESS;
    case BB_ANIM_COLOR_CYCLE: return BB_LED_CAP_RGB;
    case BB_ANIM_CHASE:
        // Force fail for count==1; the explicit count check in bb_led_anim_set
        // returns UNSUPPORTED before we reach the caps mask check.
        return (led_count > 1) ? BB_LED_CAP_RGB : (bb_led_caps_t)0xFFu;
    }
    return BB_LED_CAP_ONOFF;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_led_anim_attach(const bb_led_anim_cfg_t *cfg, bb_led_anim_handle_t *out)
{
    if (!cfg || !out || !cfg->led) return BB_ERR_INVALID_ARG;

    struct bb_led_anim *h = (struct bb_led_anim *)calloc(1, sizeof *h);
    if (!h) return BB_ERR_NO_SPACE;

    h->led            = cfg->led;
    h->tick_period_ms = cfg->tick_period_ms ? cfg->tick_period_ms : 20u;
    h->start_ms       = now_ms();
    h->last_tick_ms   = h->start_ms;
    h->paused         = false;
    h->has_pattern    = false;
    h->dirty          = false;

    *out = h;
    bb_log_i(TAG, "attached (tick=%"PRIu32"ms)", h->tick_period_ms);
    return BB_OK;
}

bb_err_t bb_led_anim_set(bb_led_anim_handle_t h, const bb_led_anim_pattern_t *pat)
{
    if (!h || !pat) return BB_ERR_INVALID_ARG;

    bb_led_caps_t caps = bb_led_caps(h->led);
    uint16_t      cnt  = bb_led_count(h->led);
    bb_led_caps_t need = pattern_required_caps(pat, cnt);

    if (pat->kind == BB_ANIM_CHASE && cnt <= 1) return BB_ERR_UNSUPPORTED;
    if ((caps & need) != need)                  return BB_ERR_UNSUPPORTED;

    h->pat          = *pat;
    h->start_ms     = now_ms();
    h->last_tick_ms = h->start_ms;
    h->has_pattern  = true;
    h->dirty        = true;
    return BB_OK;
}

bb_err_t bb_led_anim_pause(bb_led_anim_handle_t h)
{
    if (!h) return BB_ERR_INVALID_ARG;
    h->paused = true;
    return BB_OK;
}

bb_err_t bb_led_anim_resume(bb_led_anim_handle_t h)
{
    if (!h) return BB_ERR_INVALID_ARG;
    h->paused = false;
    return BB_OK;
}

bb_err_t bb_led_anim_tick(bb_led_anim_handle_t h)
{
    if (!h) return BB_ERR_INVALID_ARG;
    uint32_t n = now_ms();
    if (n - h->last_tick_ms < h->tick_period_ms) return BB_OK;
    step(h);
    return BB_OK;
}

bb_err_t bb_led_anim_detach(bb_led_anim_handle_t h)
{
    if (!h) return BB_ERR_INVALID_ARG;
    free(h);
    return BB_OK;
}
