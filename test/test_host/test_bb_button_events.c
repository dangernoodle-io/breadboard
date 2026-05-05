// Tests for bb_button_events state machine.
// Uses a bb_button_gpio mock handle and bb_button_host_inject_edge to inject
// raw PRESSED/RELEASED edges. The mock clock (BB_BUTTON_EVENTS_MOCK_CLOCK) is
// controlled via bb_button_events_set_mock_time_ms.
#include "unity.h"
#include "bb_button_events.h"
#include "bb_button_gpio.h"
#include "bb_button_gpio_host.h"
#include "bb_button_events_host.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Open a bb_button_gpio handle with 0 debounce (all edges accepted immediately).
static bb_button_handle_t open_btn(void)
{
    bb_button_gpio_cfg_t cfg = {
        .gpio        = 5,
        .active_low  = true,
        .debounce_ms = 0,
        .prefer_isr  = false,
    };
    bb_button_handle_t h = NULL;
    bb_button_gpio_open(&cfg, &h);
    return h;
}

// Open a bb_button_events handle with explicit timing and cb.
static bb_button_events_handle_t open_evt(bb_button_handle_t btn,
                                          uint16_t click_max_ms,
                                          uint16_t dbl_win_ms,
                                          uint16_t long_ms,
                                          uint16_t repeat_ms,
                                          bb_button_events_cb_t cb,
                                          void *user)
{
    bb_button_events_cfg_t cfg = {
        .button                = btn,
        .click_max_ms          = click_max_ms,
        .double_click_window_ms = dbl_win_ms,
        .long_press_ms         = long_ms,
        .repeat_interval_ms    = repeat_ms,
        .tick_period_ms        = 1,    // 1 ms so ticks always fire in tests
        .auto_start_timer      = false,
        .cb                    = cb,
        .user                  = user,
    };
    bb_button_events_handle_t h = NULL;
    bb_button_events_attach(&cfg, &h);
    return h;
}

// Advance mock clock and run a tick.
static void advance_tick(bb_button_events_handle_t h, uint32_t ms)
{
    static uint32_t s_t = 0;
    s_t += ms;
    bb_button_events_set_mock_time_ms(s_t);
    bb_button_events_tick(h);
}

// Reset the shared time counter (called from setUp per test).
static uint32_t s_time_ms = 0;

static void set_time(uint32_t ms)
{
    s_time_ms = ms;
    bb_button_events_set_mock_time_ms(ms);
}

static void tick_at(bb_button_events_handle_t h, uint32_t ms)
{
    bb_button_events_set_mock_time_ms(ms);
    bb_button_events_tick(h);
}

// ---------------------------------------------------------------------------
// Callback recorder
// ---------------------------------------------------------------------------

#define MAX_EVENTS 16

typedef struct {
    int                      count;
    bb_button_events_event_t events[MAX_EVENTS];
} evt_log_t;

static void record_cb(const bb_button_events_event_t *e, void *user)
{
    evt_log_t *log = (evt_log_t *)user;
    if (log->count < MAX_EVENTS) {
        log->events[log->count] = *e;
        log->count++;
    }
}

static evt_log_t g_log;

static void reset_log(void)
{
    memset(&g_log, 0, sizeof g_log);
}

// ---------------------------------------------------------------------------
// Tests: attach validation
// ---------------------------------------------------------------------------

void test_btn_evt_attach_null_cfg_returns_invalid_arg(void)
{
    bb_button_events_handle_t h = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_button_events_attach(NULL, &h));
    TEST_ASSERT_NULL(h);
}

void test_btn_evt_attach_null_out_returns_invalid_arg(void)
{
    bb_button_handle_t btn = open_btn();
    TEST_ASSERT_NOT_NULL(btn);
    bb_button_events_cfg_t cfg = { .button = btn };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_button_events_attach(&cfg, NULL));
    bb_button_close(btn);
}

void test_btn_evt_attach_null_button_returns_invalid_arg(void)
{
    bb_button_events_handle_t h = NULL;
    bb_button_events_cfg_t cfg = { .button = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_button_events_attach(&cfg, &h));
    TEST_ASSERT_NULL(h);
}

// ---------------------------------------------------------------------------
// Tests: CLICK (press + release within click_max, wait past double_click_window)
// ---------------------------------------------------------------------------

void test_btn_evt_single_click_emits_exactly_one_click(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 800, 100, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    // press at t=100, release at t=200 (held 100ms < click_max 200ms)
    bb_button_host_inject_edge(btn, true,  100);
    bb_button_host_inject_edge(btn, false, 200);

    // tick before double_click_window expires — no event yet
    tick_at(h, 400);
    TEST_ASSERT_EQUAL_INT(0, g_log.count);

    // tick after double_click_window (release_ms=200, window=300 → expire at 501)
    tick_at(h, 510);
    TEST_ASSERT_EQUAL_INT(1, g_log.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_EVT_CLICK, g_log.events[0].kind);

    bb_button_events_detach(h);
    bb_button_close(btn);
}

// ---------------------------------------------------------------------------
// Tests: DOUBLE_CLICK (two press/release cycles within double_click_window)
// ---------------------------------------------------------------------------

void test_btn_evt_double_click_emits_exactly_one_double_click(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 800, 100, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    // first press/release
    bb_button_host_inject_edge(btn, true,  100);
    bb_button_host_inject_edge(btn, false, 200);

    // second press within double_click_window (release_ms=200, now=350 < 200+300=500)
    bb_button_host_inject_edge(btn, true,  350);

    // should have exactly one DOUBLE_CLICK, zero CLICK
    TEST_ASSERT_EQUAL_INT(1, g_log.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_EVT_DOUBLE_CLICK, g_log.events[0].kind);

    // advance well past window; ensure no extra CLICK fires
    tick_at(h, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_log.count);

    bb_button_events_detach(h);
    bb_button_close(btn);
}

void test_btn_evt_double_click_no_click_emitted(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 800, 100, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    bb_button_host_inject_edge(btn, true,  50);
    bb_button_host_inject_edge(btn, false, 100);
    bb_button_host_inject_edge(btn, true,  250);

    // only DOUBLE_CLICK, never CLICK
    int click_count = 0;
    for (int i = 0; i < g_log.count; i++) {
        if (g_log.events[i].kind == BB_BTN_EVT_CLICK) click_count++;
    }
    TEST_ASSERT_EQUAL_INT(0, click_count);
    TEST_ASSERT_EQUAL_INT(1, g_log.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_EVT_DOUBLE_CLICK, g_log.events[0].kind);

    bb_button_events_detach(h);
    bb_button_close(btn);
}

// ---------------------------------------------------------------------------
// Tests: LONG_PRESS_START
// ---------------------------------------------------------------------------

void test_btn_evt_long_press_start_fires_once(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 500, 100, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    // press at t=100; long_press_ms=500 → fires at t=600
    bb_button_host_inject_edge(btn, true, 100);

    // tick at t=500: elapsed=400 < 500, no event
    tick_at(h, 500);
    TEST_ASSERT_EQUAL_INT(0, g_log.count);

    // tick at t=610: elapsed=510 >= 500 → LONG_PRESS_START
    tick_at(h, 610);
    TEST_ASSERT_EQUAL_INT(1, g_log.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_EVT_LONG_PRESS_START, g_log.events[0].kind);

    // another tick — still only one LONG_PRESS_START
    tick_at(h, 700);
    int lps_count = 0;
    for (int i = 0; i < g_log.count; i++) {
        if (g_log.events[i].kind == BB_BTN_EVT_LONG_PRESS_START) lps_count++;
    }
    TEST_ASSERT_EQUAL_INT(1, lps_count);

    bb_button_events_detach(h);
    bb_button_close(btn);
}

// ---------------------------------------------------------------------------
// Tests: REPEAT events with monotonically increasing held_ms
// ---------------------------------------------------------------------------

void test_btn_evt_repeat_events_monotonic_held_ms(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    // long_press_ms=500, repeat_interval_ms=150
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 500, 150, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    // press at t=30 (past default debounce guard of 25ms)
    bb_button_events_set_mock_time_ms(30);
    bb_button_host_inject_edge(btn, true, 30);

    // fire LONG_PRESS_START at t=30+510=540
    tick_at(h, 540);
    TEST_ASSERT_EQUAL_INT(1, g_log.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_EVT_LONG_PRESS_START, g_log.events[0].kind);

    // first REPEAT at t=540+150=690; also tick at 840 and 990
    tick_at(h, 690);
    tick_at(h, 840);
    tick_at(h, 990);

    // count REPEATs
    int repeat_count = 0;
    for (int i = 0; i < g_log.count; i++) {
        if (g_log.events[i].kind == BB_BTN_EVT_REPEAT) repeat_count++;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, repeat_count);

    // verify held_ms is monotonically increasing
    uint32_t prev_held = 0;
    for (int i = 0; i < g_log.count; i++) {
        if (g_log.events[i].kind == BB_BTN_EVT_REPEAT) {
            TEST_ASSERT_GREATER_OR_EQUAL_UINT32(prev_held, g_log.events[i].held_ms);
            prev_held = g_log.events[i].held_ms;
        }
    }

    bb_button_events_detach(h);
    bb_button_close(btn);
}

// ---------------------------------------------------------------------------
// Tests: LONG_PRESS_END with correct held_ms; no REPEAT after release
// ---------------------------------------------------------------------------

void test_btn_evt_long_press_end_correct_held_ms(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 500, 100, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    // press at t=30 (past default debounce guard)
    bb_button_events_set_mock_time_ms(30);
    bb_button_host_inject_edge(btn, true, 30);

    // fire LONG_PRESS_START at t=30+510=540
    tick_at(h, 540);

    // release at t=1030; held = 1030 - 30 = 1000ms
    bb_button_host_inject_edge(btn, false, 1030);

    // find LONG_PRESS_END
    int lpe_idx = -1;
    for (int i = 0; i < g_log.count; i++) {
        if (g_log.events[i].kind == BB_BTN_EVT_LONG_PRESS_END) lpe_idx = i;
    }
    TEST_ASSERT_NOT_EQUAL(-1, lpe_idx);
    TEST_ASSERT_EQUAL_UINT32(1000u, g_log.events[lpe_idx].held_ms);

    bb_button_events_detach(h);
    bb_button_close(btn);
}

void test_btn_evt_no_repeat_after_long_press_end(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 500, 100, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    bb_button_events_set_mock_time_ms(30);
    bb_button_host_inject_edge(btn, true, 30);
    tick_at(h, 540);  // fires LONG_PRESS_START
    bb_button_host_inject_edge(btn, false, 730);

    int count_before = g_log.count;

    // tick well past repeat interval; no more events expected
    tick_at(h, 900);
    tick_at(h, 1100);
    TEST_ASSERT_EQUAL_INT(count_before, g_log.count);

    bb_button_events_detach(h);
    bb_button_close(btn);
}

// ---------------------------------------------------------------------------
// Tests: medium press (release between click_max and long_press) → no event
// ---------------------------------------------------------------------------

void test_btn_evt_medium_press_no_event(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    // click_max=200, long_press=800; release at t=500 → held=500, between
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 800, 100, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    bb_button_events_set_mock_time_ms(30);
    bb_button_host_inject_edge(btn, true,  30);
    bb_button_host_inject_edge(btn, false, 530); // held 500ms: > click_max(200), < long_press(800)

    // no LONG_PRESS_START was fired, state goes to IDLE
    tick_at(h, 600);
    tick_at(h, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_log.count);

    bb_button_events_detach(h);
    bb_button_close(btn);
}

// ---------------------------------------------------------------------------
// Tests: detach — subsequent raw events do not crash; button handle still works
// ---------------------------------------------------------------------------

void test_btn_evt_detach_no_crash_on_subsequent_events(void)
{
    reset_log();
    set_time(0);
    bb_button_handle_t btn = open_btn();
    bb_button_events_handle_t h = open_evt(btn, 200, 300, 800, 100, record_cb, &g_log);
    TEST_ASSERT_NOT_NULL(h);

    bb_button_events_detach(h);

    // inject edges after detach — must not crash
    bb_button_host_inject_edge(btn, true,  100);
    bb_button_host_inject_edge(btn, false, 200);

    // caller-owned button still works
    TEST_ASSERT_FALSE(bb_button_is_pressed(btn));

    bb_button_close(btn);
}
