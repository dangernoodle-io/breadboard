// Tests for bb_button parent: debounce algorithm and callback dispatch.
#include "unity.h"
#include "bb_button.h"
#include "bb_button_gpio.h"
#include "bb_button_gpio_host.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_button_handle_t open_btn(uint16_t debounce_ms)
{
    bb_button_gpio_cfg_t cfg = {
        .gpio        = 10,
        .active_low  = true,
        .debounce_ms = debounce_ms,
        .prefer_isr  = false,
    };
    bb_button_handle_t h = NULL;
    bb_button_gpio_open(&cfg, &h);
    return h;
}

typedef struct {
    int count;
    bb_button_event_kind_t last_kind;
    uint32_t last_ts;
} cb_state_t;

static void record_cb(const bb_button_event_t *e, void *user)
{
    cb_state_t *cs = (cb_state_t *)user;
    cs->count++;
    cs->last_kind = e->kind;
    cs->last_ts   = e->timestamp_ms;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_button_open_null_cfg(void)
{
    bb_button_handle_t h = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_button_gpio_open(NULL, &h));
    TEST_ASSERT_NULL(h);
}

void test_bb_button_open_null_out(void)
{
    bb_button_gpio_cfg_t cfg = { .gpio = 10, .active_low = true, .debounce_ms = 25, .prefer_isr = false };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_button_gpio_open(&cfg, NULL));
}

void test_bb_button_press_past_debounce_fires_cb(void)
{
    bb_button_handle_t h = open_btn(25);
    TEST_ASSERT_NOT_NULL(h);

    cb_state_t cs = {0};
    bb_button_set_callback(h, record_cb, &cs);

    // Inject press at t=0 — first edge, debounce window starts.
    // last_change_ms initialises to 0, so (0 - 0) < 25 triggers the guard.
    // We need to advance past the window: inject at t=30.
    bb_button_host_inject_edge(h, true, 30);

    TEST_ASSERT_EQUAL_INT(1, cs.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_PRESSED, cs.last_kind);
    TEST_ASSERT_EQUAL_UINT32(30, cs.last_ts);

    TEST_ASSERT_TRUE(bb_button_is_pressed(h));
    bb_button_close(h);
}

void test_bb_button_second_press_within_debounce_suppressed(void)
{
    bb_button_handle_t h = open_btn(25);
    TEST_ASSERT_NOT_NULL(h);

    cb_state_t cs = {0};
    bb_button_set_callback(h, record_cb, &cs);

    // First accepted press at t=30.
    bb_button_host_inject_edge(h, true, 30);
    TEST_ASSERT_EQUAL_INT(1, cs.count);

    // Release within debounce window: t=30+10=40, delta=10 < 25, suppressed.
    bb_button_host_inject_edge(h, false, 40);
    TEST_ASSERT_EQUAL_INT(1, cs.count); // still 1, not 2

    bb_button_close(h);
}

void test_bb_button_press_then_release_fires_two_cbs(void)
{
    bb_button_handle_t h = open_btn(25);
    TEST_ASSERT_NOT_NULL(h);

    cb_state_t cs = {0};
    bb_button_set_callback(h, record_cb, &cs);

    // Press at t=30.
    bb_button_host_inject_edge(h, true, 30);
    TEST_ASSERT_EQUAL_INT(1, cs.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_PRESSED, cs.last_kind);

    // Release at t=30+30=60 (>= debounce window).
    bb_button_host_inject_edge(h, false, 60);
    TEST_ASSERT_EQUAL_INT(2, cs.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_RELEASED, cs.last_kind);

    bb_button_close(h);
}

void test_bb_button_is_pressed_reflects_debounced_state(void)
{
    bb_button_handle_t h = open_btn(25);
    TEST_ASSERT_NOT_NULL(h);

    TEST_ASSERT_FALSE(bb_button_is_pressed(h));

    bb_button_host_inject_edge(h, true, 30);
    TEST_ASSERT_TRUE(bb_button_is_pressed(h));

    bb_button_host_inject_edge(h, false, 60);
    TEST_ASSERT_FALSE(bb_button_is_pressed(h));

    bb_button_close(h);
}

void test_bb_button_get_queue_returns_null_on_host(void)
{
    bb_button_handle_t h = open_btn(25);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_NULL(bb_button_get_queue(h));
    bb_button_close(h);
}

void test_bb_button_active_low_false_high_is_press(void)
{
    // active_low=false: GPIO HIGH → pressed.
    bb_button_gpio_cfg_t cfg = {
        .gpio        = 11,
        .active_low  = false,
        .debounce_ms = 25,
        .prefer_isr  = false,
    };
    bb_button_handle_t h = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_button_gpio_open(&cfg, &h));

    cb_state_t cs = {0};
    bb_button_set_callback(h, record_cb, &cs);

    // Inject pressed=true (raw_pressed=true) at t=30.
    bb_button_host_inject_edge(h, true, 30);
    TEST_ASSERT_EQUAL_INT(1, cs.count);
    TEST_ASSERT_EQUAL_INT(BB_BTN_PRESSED, cs.last_kind);

    bb_button_close(h);
}

void test_bb_button_close_subsequent_inject_noop(void)
{
    bb_button_handle_t h = open_btn(25);
    TEST_ASSERT_NOT_NULL(h);

    cb_state_t cs = {0};
    bb_button_set_callback(h, record_cb, &cs);

    bb_button_close(h);

    // After close, inject should not crash or fire cb.
    bb_button_host_inject_edge(h, true, 100);
    // cs.count remains 0 — no crash, no callback.
    TEST_ASSERT_EQUAL_INT(0, cs.count);
}
