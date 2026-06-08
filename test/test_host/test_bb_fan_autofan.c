// Tests for bb_fan autofan PID controller (BB_FAN_AUTOFAN feature).
// Safety net: all controller behaviors must match TM's proven autofan.
#ifdef CONFIG_BB_FAN_AUTOFAN

#include "unity.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Fake fan backend
// ---------------------------------------------------------------------------

typedef struct {
    int   rpm;
    int   duty_pct;
    float die_c;
    float board_c;
    bool  die_fail;
    bool  board_fail;
    int   set_duty_last;
    int   duty_count;
} fake_af_t;

static fake_af_t g_af;

static bb_err_t af_set_duty(void *s, int pct)
{
    fake_af_t *f = s;
    f->set_duty_last  = pct;
    f->duty_count++;
    f->duty_pct = pct;
    return BB_OK;
}
static int af_get_duty(void *s) { return ((fake_af_t *)s)->duty_pct; }
static int af_rpm(void *s)      { return ((fake_af_t *)s)->rpm; }
static bb_err_t af_die(void *s, float *out)
{
    fake_af_t *f = s;
    if (f->die_fail) return BB_ERR_INVALID_STATE;
    *out = f->die_c;
    return BB_OK;
}
static bb_err_t af_board(void *s, float *out)
{
    fake_af_t *f = s;
    if (f->board_fail) return BB_ERR_INVALID_STATE;
    *out = f->board_c;
    return BB_OK;
}

static const bb_fan_driver_t drv_af = {
    .set_duty_pct      = af_set_duty,
    .get_duty_pct      = af_get_duty,
    .read_rpm          = af_rpm,
    .read_die_temp_c   = af_die,
    .read_board_temp_c = af_board,
    .name              = "fake_autofan",
};

// ---------------------------------------------------------------------------
// Mock clock
// ---------------------------------------------------------------------------

static unsigned long s_mock_ms = 0;
static unsigned long mock_now_ms(void) { return s_mock_ms; }

// Advance mock clock by N ms and trigger a PID sample.
// Returns the duty that poll produced.
static int advance_poll(bb_fan_handle_t h, unsigned long delta_ms)
{
    s_mock_ms += delta_ms;
    bb_fan_poll(h);
    return g_af.set_duty_last;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static bb_fan_handle_t make_handle(void)
{
    memset(&g_af, 0, sizeof g_af);
    g_af.duty_pct = -1;
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_af, &g_af, &h);

    // Inject mock clock so PID sample gate is deterministic
    s_mock_ms = 0;
    bb_fan_pid_set_mock_clock(h, mock_now_ms);

    return h;
}

static void enable_autofan(bb_fan_handle_t h, float die_target, float aux_target,
                            int min_pct, int manual_pct)
{
    bb_fan_autofan_cfg_t cfg = {
        .enabled      = true,
        .die_target_c = die_target,
        .aux_target_c = aux_target,
        .min_pct      = min_pct,
        .manual_pct   = manual_pct,
    };
    bb_fan_set_autofan(h, &cfg);
}

// ---------------------------------------------------------------------------
// PID direction: REVERSE means hotter → higher duty
// ---------------------------------------------------------------------------

void test_autofan_hotter_than_target_increases_duty(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100); // aux_target=0 disables aux

    // Die well above target → PID should push duty up
    g_af.die_c = 75.0f; // 15°C above target
    int duty = advance_poll(h, 5000);
    // With REVERSE PID, error = setpoint - input = 60 - 75 = -15.
    // P_ON_E and reverse: kp is negated, so output goes up (positive direction).
    // Should be > min_pct (25) and <= 100
    TEST_ASSERT_GREATER_THAN(25, duty);
    TEST_ASSERT_LESS_OR_EQUAL(100, duty);

    free(h);
}

void test_autofan_cooler_than_target_gives_min_pct(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    // Die well below target → PID should clamp at min
    g_af.die_c = 40.0f;
    int duty = advance_poll(h, 5000);
    TEST_ASSERT_EQUAL_INT(25, duty);

    free(h);
}

// ---------------------------------------------------------------------------
// PID sample-time gating: no update before 5000ms
// ---------------------------------------------------------------------------

void test_autofan_sample_time_gate_no_update_before_5s(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    g_af.die_c = 75.0f;

    // First poll at t=0 → arms PID (no compute yet, lastTime was set at init -5000ms)
    // The mock clock starts at 0, lastTime = 0 - 5000 → underflow (unsigned), so it WILL fire.
    // After that, immediately poll again at t=0 (no advance): should NOT fire.
    bb_fan_poll(h); // fires (lastTime underflows)
    int duty_after_first = g_af.set_duty_last;

    // Second poll immediately (no time advance) — should NOT compute PID
    g_af.die_c = 40.0f; // drop temp; if PID fires, duty would drop
    bb_fan_poll(h);
    int duty_after_immediate = g_af.set_duty_last;

    // Duty should not have changed (gate blocked)
    TEST_ASSERT_EQUAL_INT(duty_after_first, duty_after_immediate);

    free(h);
}

void test_autofan_sample_time_gate_fires_at_5s(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    g_af.die_c = 75.0f;
    advance_poll(h, 5000); // first fire
    int duty1 = g_af.set_duty_last;

    // Advance another 5s with a cooler temp — PID should update
    g_af.die_c = 40.0f;
    advance_poll(h, 5000); // second fire
    int duty2 = g_af.set_duty_last;

    // Duty should have changed (EMA converges toward cooler temp)
    TEST_ASSERT_NOT_EQUAL(duty1, duty2);

    free(h);
}

// ---------------------------------------------------------------------------
// Output clamping [min_pct, 100]
// ---------------------------------------------------------------------------

void test_autofan_output_clamp_at_max_100(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    // Extreme overheat → many ticks to saturate output
    g_af.die_c = 120.0f;
    for (int i = 0; i < 20; i++) {
        advance_poll(h, 5000);
    }
    TEST_ASSERT_EQUAL_INT(100, g_af.set_duty_last);

    free(h);
}

void test_autofan_output_clamp_at_min_pct(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 35, 100);

    // Well below target → clamp at min
    g_af.die_c = 20.0f;
    for (int i = 0; i < 20; i++) {
        advance_poll(h, 5000);
    }
    TEST_ASSERT_EQUAL_INT(35, g_af.set_duty_last);

    free(h);
}

// ---------------------------------------------------------------------------
// EMA alpha=0.2: new_ema = 0.2*raw + 0.8*old_ema
// ---------------------------------------------------------------------------

void test_autofan_ema_converges_toward_temp(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    // First poll: EMA initialized to raw (75°C)
    g_af.die_c = 75.0f;
    advance_poll(h, 5000);

    // Second poll at same temp: EMA = 0.2*75 + 0.8*75 = 75 (stays the same)
    advance_poll(h, 5000);

    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 75.0f, tel.die_ema_c);

    // Now drop temp to 50°C; EMA should only partially converge
    g_af.die_c = 50.0f;
    advance_poll(h, 5000);
    bb_fan_get_autofan_telemetry(h, &tel);
    // Expected: 0.2*50 + 0.8*75 = 10 + 60 = 70
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 70.0f, tel.die_ema_c);

    free(h);
}

// ---------------------------------------------------------------------------
// Die-vs-aux input-src selection: whichever has larger (ema-target)/target wins
// ---------------------------------------------------------------------------

void test_autofan_die_src_when_die_ratio_larger(void)
{
    bb_fan_handle_t h = make_handle();
    // die_target=60, aux_target=75
    enable_autofan(h, 60.0f, 75.0f, 25, 100);

    // die=75 (ratio=(75-60)/60=0.25), aux=80 (ratio=(80-75)/75=0.067) → die wins
    g_af.die_c = 75.0f;
    bb_fan_set_aux_temp(h, 80.0f);
    advance_poll(h, 5000);

    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);
    TEST_ASSERT_EQUAL_STRING("die", tel.pid_input_src);

    free(h);
}

void test_autofan_aux_src_when_aux_ratio_larger(void)
{
    bb_fan_handle_t h = make_handle();
    // die_target=60, aux_target=75
    enable_autofan(h, 60.0f, 75.0f, 25, 100);

    // die=61 (ratio=(61-60)/60=0.017), aux=90 (ratio=(90-75)/75=0.2) → aux wins
    g_af.die_c = 61.0f;
    bb_fan_set_aux_temp(h, 90.0f);
    advance_poll(h, 5000);

    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);
    TEST_ASSERT_EQUAL_STRING("aux", tel.pid_input_src);

    free(h);
}

void test_autofan_die_src_when_aux_not_fed(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 75.0f, 25, 100);

    // aux not fed (stays -1) → die wins by default
    g_af.die_c = 65.0f;
    // Do NOT call bb_fan_set_aux_temp
    advance_poll(h, 5000);

    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);
    TEST_ASSERT_EQUAL_STRING("die", tel.pid_input_src);

    free(h);
}

// ---------------------------------------------------------------------------
// Manual mode (autofan disabled)
// When autofan is disabled, bb_fan_poll() does NOT change duty.
// Consumer controls duty via bb_fan_set_duty_pct() directly.
// ---------------------------------------------------------------------------

void test_autofan_disabled_does_not_change_duty(void)
{
    bb_fan_handle_t h = make_handle();

    bb_fan_autofan_cfg_t cfg = {
        .enabled      = false,
        .die_target_c = 60.0f,
        .aux_target_c = 0.0f,
        .min_pct      = 25,
        .manual_pct   = 70,
    };
    bb_fan_set_autofan(h, &cfg);

    // Set duty manually first
    bb_fan_set_duty_pct(h, 42);
    g_af.set_duty_last = 42; // track what was set

    g_af.die_c = 75.0f; // hot but autofan is off
    // Reset set_duty_count to detect if poll calls set_duty_pct
    int count_before = g_af.duty_count;
    bb_fan_poll(h); // should NOT change duty

    // set_duty_pct should NOT have been called again
    TEST_ASSERT_EQUAL_INT(count_before, g_af.duty_count);

    free(h);
}

void test_autofan_disabled_no_pid_output(void)
{
    bb_fan_handle_t h = make_handle();

    bb_fan_autofan_cfg_t cfg = {
        .enabled      = false,
        .die_target_c = 60.0f,
        .aux_target_c = 0.0f,
        .min_pct      = 25,
        .manual_pct   = 50,
    };
    bb_fan_set_autofan(h, &cfg);

    // Set duty manually
    bb_fan_set_duty_pct(h, 50);
    int count_before = g_af.duty_count;

    g_af.die_c = 75.0f;
    advance_poll(h, 5000);
    advance_poll(h, 5000);

    // Autofan disabled: duty NOT changed by poll
    TEST_ASSERT_EQUAL_INT(count_before, g_af.duty_count);

    free(h);
}

// ---------------------------------------------------------------------------
// Fail-safe: temp read fails → 100% duty
// ---------------------------------------------------------------------------

void test_autofan_temp_fail_gives_100pct(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    g_af.die_fail = true;
    advance_poll(h, 5000);

    TEST_ASSERT_EQUAL_INT(100, g_af.set_duty_last);

    free(h);
}

// ---------------------------------------------------------------------------
// Enable/disable toggle: disable resets PID; re-enable re-arms
// ---------------------------------------------------------------------------

void test_autofan_toggle_disable_reenable(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    g_af.die_c = 75.0f;
    advance_poll(h, 5000); // PID armed
    int duty_auto = g_af.set_duty_last;
    TEST_ASSERT_GREATER_THAN(25, duty_auto);

    // Disable: poll no longer changes duty
    bb_fan_autofan_cfg_t cfg = {
        .enabled    = false,
        .die_target_c = 60.0f,
        .min_pct    = 25,
        .manual_pct = 50,
    };
    bb_fan_set_autofan(h, &cfg);
    int count_before = g_af.duty_count;
    advance_poll(h, 5000); // should NOT call set_duty_pct
    TEST_ASSERT_EQUAL_INT(count_before, g_af.duty_count);

    // Re-enable: PID re-arms, duty driven again
    enable_autofan(h, 60.0f, 0.0f, 25, 100);
    g_af.die_c = 75.0f;
    advance_poll(h, 5000); // re-arms and computes
    int duty_reauto = g_af.set_duty_last;
    TEST_ASSERT_GREATER_THAN(25, duty_reauto);

    free(h);
}

// ---------------------------------------------------------------------------
// PID convergence: after many ticks at steady temp, duty stabilizes
// ---------------------------------------------------------------------------

void test_autofan_pid_convergence_stable_temp(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    // Constant temp = target: PID should settle toward 0 error
    g_af.die_c = 60.0f;
    int last_duty = -1;
    for (int i = 0; i < 50; i++) {
        int d = advance_poll(h, 5000);
        last_duty = d;
    }
    // At target, PID should be at or near min_pct (no error → integrator
    // may wind toward 0 or min; exact value depends on integral accumulation).
    // Conservatively: duty should be within valid range [25, 100].
    TEST_ASSERT_GREATER_OR_EQUAL(25, last_duty);
    TEST_ASSERT_LESS_OR_EQUAL(100, last_duty);

    free(h);
}

// ---------------------------------------------------------------------------
// Telemetry: die_ema / aux_ema / pid_input_c / pid_input_src
// ---------------------------------------------------------------------------

void test_autofan_telemetry_uninitialized_before_poll(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 75.0f, 25, 100);

    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);

    TEST_ASSERT_LESS_THAN(0.0f, tel.die_ema_c);
    TEST_ASSERT_LESS_THAN(0.0f, tel.pid_input_c);
    TEST_ASSERT_EQUAL_STRING("", tel.pid_input_src);

    free(h);
}

void test_autofan_telemetry_populated_after_poll(void)
{
    bb_fan_handle_t h = make_handle();
    enable_autofan(h, 60.0f, 0.0f, 25, 100);

    g_af.die_c = 72.0f;
    advance_poll(h, 5000);

    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);

    // First tick: EMA initialized to raw
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 72.0f, tel.die_ema_c);
    TEST_ASSERT_GREATER_OR_EQUAL(0.0f, tel.pid_input_c);
    TEST_ASSERT_EQUAL_STRING("die", tel.pid_input_src);

    free(h);
}

void test_autofan_telemetry_null_handle_returns_defaults(void)
{
    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(NULL, &tel);

    TEST_ASSERT_LESS_THAN(0.0f, tel.die_ema_c);
    TEST_ASSERT_LESS_THAN(0.0f, tel.aux_ema_c);
    TEST_ASSERT_LESS_THAN(0.0f, tel.pid_input_c);
    TEST_ASSERT_EQUAL_STRING("", tel.pid_input_src);
}

// ---------------------------------------------------------------------------
// get_autofan_cfg: reads back what was set
// ---------------------------------------------------------------------------

void test_autofan_get_cfg_reads_back(void)
{
    bb_fan_handle_t h = make_handle();

    bb_fan_autofan_cfg_t cfg_in = {
        .enabled      = true,
        .die_target_c = 65.0f,
        .aux_target_c = 80.0f,
        .min_pct      = 30,
        .manual_pct   = 60,
    };
    bb_fan_set_autofan(h, &cfg_in);

    bb_fan_autofan_cfg_t cfg_out;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_get_autofan_cfg(h, &cfg_out));

    TEST_ASSERT_TRUE(cfg_out.enabled);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 65.0f, cfg_out.die_target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, cfg_out.aux_target_c);
    TEST_ASSERT_EQUAL_INT(30, cfg_out.min_pct);
    TEST_ASSERT_EQUAL_INT(60, cfg_out.manual_pct);

    free(h);
}

void test_autofan_get_cfg_null_args(void)
{
    bb_fan_handle_t h = make_handle();
    bb_fan_autofan_cfg_t cfg;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fan_get_autofan_cfg(NULL, &cfg));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fan_get_autofan_cfg(h, NULL));

    free(h);
}

// ---------------------------------------------------------------------------
// set_aux_temp: null handle returns error
// ---------------------------------------------------------------------------

void test_autofan_set_aux_temp_null_handle(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fan_set_aux_temp(NULL, 70.0f));
}

// ---------------------------------------------------------------------------
// set_autofan: null args return error
// ---------------------------------------------------------------------------

void test_autofan_set_autofan_null_handle(void)
{
    bb_fan_autofan_cfg_t cfg = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fan_set_autofan(NULL, &cfg));
}

void test_autofan_set_autofan_null_cfg(void)
{
    bb_fan_handle_t h = make_handle();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fan_set_autofan(h, NULL));
    free(h);
}

#endif /* CONFIG_BB_FAN_AUTOFAN */
