// Tests for bb_led_anim pattern/animation library.
// Uses a mock bb_led driver (via bb_led_handle_create) and a mock clock
// (BB_LED_ANIM_MOCK_CLOCK + bb_led_anim_set_mock_time_ms).
#include "unity.h"
#include "bb_led_anim.h"
#include "bb_led_driver.h"
#include "bb_led_anim_host.h"
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Mock bb_led driver
// ---------------------------------------------------------------------------

#define MOCK_COUNT 4

typedef struct {
    bool     on[MOCK_COUNT];
    uint8_t  pct[MOCK_COUNT];
    uint16_t level[MOCK_COUNT];
    uint8_t  r[MOCK_COUNT], g[MOCK_COUNT], b[MOCK_COUNT];
    int      set_on_calls;
    int      set_brightness_calls;
    int      set_level_calls;
    int      set_color_calls;
    int      flush_calls;
    bool     closed;
} anim_mock_state_t;

static anim_mock_state_t g_mock;

static bb_err_t m_set_on(void *s, uint16_t i, bool v)
{
    anim_mock_state_t *m = (anim_mock_state_t *)s;
    m->on[i] = v;
    m->set_on_calls++;
    return BB_OK;
}
static bb_err_t m_set_brightness(void *s, uint16_t i, uint8_t p)
{
    anim_mock_state_t *m = (anim_mock_state_t *)s;
    m->pct[i] = p;
    m->set_brightness_calls++;
    return BB_OK;
}
static bb_err_t m_set_level(void *s, uint16_t i, uint16_t l)
{
    anim_mock_state_t *m = (anim_mock_state_t *)s;
    m->level[i] = l;
    m->set_level_calls++;
    return BB_OK;
}
static bb_err_t m_set_color(void *s, uint16_t i, uint8_t r, uint8_t g, uint8_t b)
{
    anim_mock_state_t *m = (anim_mock_state_t *)s;
    m->r[i] = r; m->g[i] = g; m->b[i] = b;
    m->set_color_calls++;
    return BB_OK;
}
static bb_err_t m_flush(void *s)
{
    ((anim_mock_state_t *)s)->flush_calls++;
    return BB_OK;
}
static bb_err_t m_close(void *s)
{
    ((anim_mock_state_t *)s)->closed = true;
    return BB_OK;
}

// Full-caps driver (ONOFF | BRIGHTNESS | RGB, count=4)
static const bb_led_driver_t s_drv_full = {
    .set_on = m_set_on, .set_brightness = m_set_brightness,
    .set_color = m_set_color, .flush = m_flush, .close = m_close,
    .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB,
    .count = MOCK_COUNT,
};

// ONOFF-only driver (count=1)
static const bb_led_driver_t s_drv_onoff = {
    .set_on = m_set_on, .set_brightness = m_set_brightness,
    .set_color = m_set_color, .flush = m_flush, .close = m_close,
    .caps  = BB_LED_CAP_ONOFF,
    .count = 1,
};

// BRIGHTNESS driver (count=1)
static const bb_led_driver_t s_drv_brightness = {
    .set_on = m_set_on, .set_brightness = m_set_brightness,
    .set_color = m_set_color, .flush = m_flush, .close = m_close,
    .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS,
    .count = 1,
};

// BRIGHTNESS driver WITH the fine set_level op (count=1) — captures 16-bit levels.
static const bb_led_driver_t s_drv_level = {
    .set_on = m_set_on, .set_brightness = m_set_brightness, .set_level = m_set_level,
    .set_color = m_set_color, .flush = m_flush, .close = m_close,
    .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS,
    .count = 1,
};

// Reset helper called by setUp
void bb_led_anim_test_reset(void)
{
    memset(&g_mock, 0, sizeof g_mock);
    bb_led_anim_set_mock_time_ms(0);
}

// ---------------------------------------------------------------------------
// Helper: open a led handle + attach an animator in one call
// ---------------------------------------------------------------------------

static bb_led_handle_t open_led(const bb_led_driver_t *drv)
{
    bb_led_handle_t h = NULL;
    bb_led_handle_create(drv, &g_mock, &h);
    return h;
}

static bb_led_anim_handle_t attach(bb_led_handle_t led)
{
    bb_led_anim_cfg_t cfg = { .led = led, .tick_period_ms = 0, .auto_start_timer = false };
    bb_led_anim_handle_t ah = NULL;
    bb_led_anim_attach(&cfg, &ah);
    return ah;
}

// Force a tick ignoring the period guard (set time forward by 1 second first).
static void force_tick(bb_led_anim_handle_t ah)
{
    static uint32_t s_t = 0;
    s_t += 1000u;
    bb_led_anim_set_mock_time_ms(s_t);
    bb_led_anim_tick(ah);
}

// ---------------------------------------------------------------------------
// Tests: attach / detach
// ---------------------------------------------------------------------------

void test_anim_attach_null_cfg_returns_invalid_arg(void)
{
    bb_led_anim_handle_t ah;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_anim_attach(NULL, &ah));
}

void test_anim_attach_null_out_returns_invalid_arg(void)
{
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_cfg_t cfg = { .led = led, .tick_period_ms = 0, .auto_start_timer = false };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_anim_attach(&cfg, NULL));
    bb_led_close(led);
}

void test_anim_detach_no_crash(void)
{
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_handle_t ah = attach(led);
    TEST_ASSERT_NOT_NULL(ah);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_detach(ah));
    bb_led_close(led);
}

// ---------------------------------------------------------------------------
// Tests: bb_led_anim_set caps gating
// ---------------------------------------------------------------------------

void test_anim_set_solid_on_onoff_handle_returns_ok(void)
{
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_SOLID };
    pat.solid.r = 0; pat.solid.g = 0; pat.solid.b = 0; pat.solid.brightness_pct = 100;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_set(ah, &pat));

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

void test_anim_set_rgb_pattern_on_onoff_handle_returns_unsupported(void)
{
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_COLOR_CYCLE };
    pat.color_cycle.period_ms = 1000; pat.color_cycle.sat_pct = 100; pat.color_cycle.val_pct = 100;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_led_anim_set(ah, &pat));

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

void test_anim_set_chase_on_single_led_returns_unsupported(void)
{
    bb_led_handle_t led = open_led(&s_drv_full);  // full caps but count=4 → OK
    // Use a single-count full-caps driver for chase failure
    // Re-use s_drv_onoff which has count=1 → chase must fail
    bb_led_handle_t led1 = open_led(&s_drv_onoff);
    bb_led_anim_handle_t ah = attach(led1);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_CHASE };
    pat.chase.period_ms = 1000; pat.chase.r = 255; pat.chase.g = 0; pat.chase.b = 0;
    pat.chase.tail_len = 2;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_led_anim_set(ah, &pat));

    bb_led_anim_detach(ah);
    bb_led_close(led);
    bb_led_close(led1);
}

// ---------------------------------------------------------------------------
// Tests: BLINK
// ---------------------------------------------------------------------------

void test_anim_blink_on_at_quarter_period(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_BLINK };
    pat.blink.period_ms = 1000; pat.blink.duty_pct = 50;
    bb_led_anim_set(ah, &pat);  // resets start_ms to 0

    // t=250ms → phase=250, 250 < 500 → ON
    bb_led_anim_set_mock_time_ms(250);
    bb_led_anim_tick(ah);
    TEST_ASSERT_TRUE(g_mock.on[0]);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

void test_anim_blink_off_at_three_quarter_period(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_BLINK };
    pat.blink.period_ms = 1000; pat.blink.duty_pct = 50;
    bb_led_anim_set(ah, &pat);

    // t=750ms → phase=750, 750 >= 500 → OFF
    bb_led_anim_set_mock_time_ms(750);
    bb_led_anim_tick(ah);
    TEST_ASSERT_FALSE(g_mock.on[0]);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// level_pct > 0: blink flashes via set_level at that brightness (not full on/off).
void test_anim_blink_level_pct_flashes_at_level(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_level);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_BLINK };
    pat.blink.period_ms = 1000; pat.blink.duty_pct = 50; pat.blink.level_pct = 25;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_set(ah, &pat));

    // ON phase → set_level to ~25% (25*65535/100 = 16383), not set_on.
    bb_led_anim_set_mock_time_ms(250);
    bb_led_anim_tick(ah);
    TEST_ASSERT_EQUAL_UINT16(16383, g_mock.level[0]);
    TEST_ASSERT_TRUE(g_mock.set_level_calls > 0);
    TEST_ASSERT_EQUAL_INT(0, g_mock.set_on_calls);   // never used the on/off path

    // OFF phase → level 0.
    bb_led_anim_set_mock_time_ms(750);
    bb_led_anim_tick(ah);
    TEST_ASSERT_EQUAL_UINT16(0, g_mock.level[0]);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// level_pct > 0 needs BRIGHTNESS; on an ONOFF-only handle it must be unsupported.
void test_anim_blink_level_pct_unsupported_on_onoff(void)
{
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_BLINK };
    pat.blink.period_ms = 1000; pat.blink.duty_pct = 50; pat.blink.level_pct = 25;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_led_anim_set(ah, &pat));

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// ---------------------------------------------------------------------------
// Tests: BREATHE
// ---------------------------------------------------------------------------

void test_anim_breathe_brightness_rises_then_falls(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_brightness);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_BREATHE };
    pat.breathe.period_ms = 1000; pat.breathe.min_pct = 10; pat.breathe.max_pct = 100;
    bb_led_anim_set(ah, &pat);

    // Sample at t=20(≈0), t=250, t=750.
    // sin LUT: phase 0→midpoint(128), phase 64→peak(255), phase 192→trough(0).
    // With period=1000: idx at t=250 is ~64 (near peak), at t=750 is ~192 (near trough).
    uint8_t pct_start, pct_peak, pct_trough;

    // t≈0: elapsed≈20ms, sin_idx≈5, sin8≈143 (slightly above mid 128)
    bb_led_anim_set_mock_time_ms(20);
    bb_led_anim_tick(ah);
    pct_start = g_mock.pct[0];

    // t=250: elapsed=250ms, sin_idx=64, sin8≈254 (peak)
    bb_led_anim_set_mock_time_ms(250);
    bb_led_anim_tick(ah);
    pct_peak = g_mock.pct[0];

    // t=750: elapsed=750ms, sin_idx=192, sin8≈1 (trough)
    bb_led_anim_set_mock_time_ms(750);
    bb_led_anim_tick(ah);
    pct_trough = g_mock.pct[0];

    // Peak should be above start; trough should be below start.
    TEST_ASSERT_GREATER_THAN_UINT8(pct_start, pct_peak);
    TEST_ASSERT_GREATER_THAN_UINT8(pct_trough, pct_start);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// ---------------------------------------------------------------------------
// Tests: COLOR_CYCLE
// ---------------------------------------------------------------------------

void test_anim_color_cycle_red_dominant_at_hue_zero(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_full);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_COLOR_CYCLE };
    pat.color_cycle.period_ms = 3000;
    pat.color_cycle.sat_pct   = 100;
    pat.color_cycle.val_pct   = 100;
    bb_led_anim_set(ah, &pat);

    // Advance by one tick period so the guard passes; elapsed is still near 0
    // relative to the 3000ms period → hue ≈ 0 → red dominant.
    bb_led_anim_set_mock_time_ms(20);
    bb_led_anim_tick(ah);
    TEST_ASSERT_GREATER_THAN_UINT8(100, g_mock.r[0]);
    TEST_ASSERT_LESS_THAN_UINT8(50, g_mock.g[0]);
    TEST_ASSERT_LESS_THAN_UINT8(50, g_mock.b[0]);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

void test_anim_color_cycle_green_dominant_at_one_third_period(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_full);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_COLOR_CYCLE };
    pat.color_cycle.period_ms = 3000;
    pat.color_cycle.sat_pct   = 100;
    pat.color_cycle.val_pct   = 100;
    bb_led_anim_set(ah, &pat);

    // t=1000 → hue=120 → green dominant
    bb_led_anim_set_mock_time_ms(1000);
    bb_led_anim_tick(ah);
    TEST_ASSERT_GREATER_THAN_UINT8(100, g_mock.g[0]);
    TEST_ASSERT_LESS_THAN_UINT8(50, g_mock.r[0]);
    TEST_ASSERT_LESS_THAN_UINT8(50, g_mock.b[0]);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

void test_anim_color_cycle_blue_dominant_at_two_thirds_period(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_full);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_COLOR_CYCLE };
    pat.color_cycle.period_ms = 3000;
    pat.color_cycle.sat_pct   = 100;
    pat.color_cycle.val_pct   = 100;
    bb_led_anim_set(ah, &pat);

    // t=2000 → hue=240 → blue dominant
    bb_led_anim_set_mock_time_ms(2000);
    bb_led_anim_tick(ah);
    TEST_ASSERT_GREATER_THAN_UINT8(100, g_mock.b[0]);
    TEST_ASSERT_LESS_THAN_UINT8(50, g_mock.r[0]);
    TEST_ASSERT_LESS_THAN_UINT8(50, g_mock.g[0]);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// ---------------------------------------------------------------------------
// Tests: DETACH
// ---------------------------------------------------------------------------

void test_anim_detach_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_anim_detach(NULL));
}

// ---------------------------------------------------------------------------
// Tests: auto_start_timer
// ---------------------------------------------------------------------------

// auto_start=true: timer fires periodically and drives the animation step.
// Use a 50ms period; set a solid pattern (will flush on first callback since
// dirty=true), then sleep long enough for at least one callback to land.
void test_anim_auto_start_timer_fires(void)
{
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_cfg_t cfg = { .led = led, .tick_period_ms = 50, .auto_start_timer = true };
    bb_led_anim_handle_t ah = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_attach(&cfg, &ah));
    TEST_ASSERT_NOT_NULL(ah);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_SOLID };
    pat.solid.r = 0; pat.solid.g = 0; pat.solid.b = 0; pat.solid.brightness_pct = 100;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_set(ah, &pat));

    // Wait for at least two timer periods.
    usleep(200000);

    TEST_ASSERT_GREATER_THAN(0, g_mock.flush_calls);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// auto_start=false: without explicit tick() calls no step fires.
void test_anim_no_auto_start_timer_does_not_fire(void)
{
    memset(&g_mock, 0, sizeof g_mock);
    bb_led_handle_t led = open_led(&s_drv_onoff);
    bb_led_anim_cfg_t cfg = { .led = led, .tick_period_ms = 20, .auto_start_timer = false };
    bb_led_anim_handle_t ah = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_attach(&cfg, &ah));
    TEST_ASSERT_NOT_NULL(ah);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_SOLID };
    pat.solid.r = 0; pat.solid.g = 0; pat.solid.b = 0; pat.solid.brightness_pct = 100;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_set(ah, &pat));

    // Sleep without calling tick() — no flush should occur.
    usleep(100000);

    TEST_ASSERT_EQUAL(0, g_mock.flush_calls);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// Regression for B1-243: a dim 0–5% breathe used to render only ~6 integer-pct
// levels. The 16-bit set_level path must sweep many distinct levels.
void test_anim_breathe_dim_has_many_levels(void)
{
    bb_led_handle_t led = open_led(&s_drv_level);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t pat = { .kind = BB_ANIM_BREATHE };
    pat.breathe.period_ms = 2000;
    pat.breathe.min_pct   = 0;
    pat.breathe.max_pct   = 5;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_set(ah, &pat));

    // Sample levels across a full breathe period (50 ms steps > 20 ms tick guard).
    uint16_t seen[64];
    int n_seen = 0;
    for (uint32_t t = 0; t <= 2000; t += 50) {
        bb_led_anim_set_mock_time_ms(t);
        bb_led_anim_tick(ah);
        uint16_t lv = g_mock.level[0];
        bool found = false;
        for (int j = 0; j < n_seen; j++) if (seen[j] == lv) { found = true; break; }
        if (!found && n_seen < 64) seen[n_seen++] = lv;
    }

    TEST_ASSERT_TRUE(g_mock.set_level_calls > 0);   // used the fine path, not the bridge
    TEST_ASSERT_TRUE(n_seen >= 20);                 // smooth, not ~6 steps

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// ---------------------------------------------------------------------------
// Tests: transition / fade handoff
// ---------------------------------------------------------------------------

// A boot solid 50% should fade INTO a dim breathe over fade_ms — mid-fade the
// level is pulled up toward the 50% source (well above the breathe ceiling), and
// after the window it settles onto the pure breathe.
void test_anim_set_transition_fades_from_current_level(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_level);
    bb_led_anim_handle_t ah = attach(led);

    // Boot solid 50% → last_level = 50% of 65535 = 32767.
    bb_led_anim_pattern_t solid = { .kind = BB_ANIM_SOLID };
    solid.solid.brightness_pct = 50;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_set(ah, &solid));
    bb_led_anim_set_mock_time_ms(20);
    bb_led_anim_tick(ah);
    uint16_t solid_level = g_mock.level[0];
    TEST_ASSERT_EQUAL_UINT16(32767, solid_level);
    // A brightness LED must NOT also get set_on(true) for solid — that would
    // force full duty and clobber the 50% on real PWM hardware.
    TEST_ASSERT_EQUAL_INT(0, g_mock.set_on_calls);

    // Transition into a dim 1–10% breathe over 800 ms, starting at t=1000.
    bb_led_anim_set_mock_time_ms(1000);
    bb_led_anim_pattern_t breathe = { .kind = BB_ANIM_BREATHE };
    breathe.breathe.period_ms = 3000; breathe.breathe.min_pct = 1; breathe.breathe.max_pct = 10;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_set_transition(ah, &breathe, 800));

    uint16_t breathe_ceiling = (uint16_t)(10u * 65535u / 100u);   // 6553

    // Mid-fade (t=1400, 50% through): blended level sits between the breathe
    // target and the 50% source — proving the fade, not the raw pattern, drives it.
    bb_led_anim_set_mock_time_ms(1400);
    bb_led_anim_tick(ah);
    uint16_t mid = g_mock.level[0];
    TEST_ASSERT_TRUE(mid > breathe_ceiling);   // still pulled up by the fade
    TEST_ASSERT_TRUE(mid < solid_level);       // and below the 50% source

    // Past the fade window (t=1900): pure breathe, at/under its 10% ceiling.
    bb_led_anim_set_mock_time_ms(1900);
    bb_led_anim_tick(ah);
    TEST_ASSERT_TRUE(g_mock.level[0] <= breathe_ceiling + 1);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}

// fade_ms 0 behaves exactly like bb_led_anim_set (no blend): the very first tick
// of the new pattern shows its raw value, not a blend with the prior level.
void test_anim_set_transition_zero_fade_is_instant(void)
{
    bb_led_anim_set_mock_time_ms(0);
    bb_led_handle_t led = open_led(&s_drv_level);
    bb_led_anim_handle_t ah = attach(led);

    bb_led_anim_pattern_t solid = { .kind = BB_ANIM_SOLID };
    solid.solid.brightness_pct = 50;
    bb_led_anim_set(ah, &solid);
    bb_led_anim_set_mock_time_ms(20);
    bb_led_anim_tick(ah);

    // Transition to a SOLID 5% with fade_ms 0 → immediate 5% (3276), not a blend.
    bb_led_anim_set_mock_time_ms(100);
    bb_led_anim_pattern_t low = { .kind = BB_ANIM_SOLID };
    low.solid.brightness_pct = 5;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_anim_set_transition(ah, &low, 0));
    bb_led_anim_set_mock_time_ms(120);
    bb_led_anim_tick(ah);
    TEST_ASSERT_EQUAL_UINT16(3276, g_mock.level[0]);

    bb_led_anim_detach(ah);
    bb_led_close(led);
}
