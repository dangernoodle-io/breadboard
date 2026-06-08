// Unit tests for bb_fan_pid internals — covers branches not reachable via the
// high-level autofan integration tests (set_clock null, P_ON_M, DIRECT direction,
// negative-gains guard, set_sample_time(0), set_output_limits invalid,
// set_direction while armed, initialize with outputSum>outMax, get_mode).
#ifdef CONFIG_BB_FAN_AUTOFAN

#include "unity.h"
#include "../../../components/bb_fan/src/bb_fan_pid.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Helper: init a PID with common defaults, inject clock via fn pointer
// ---------------------------------------------------------------------------

static unsigned long s_pid_ms = 0;
static unsigned long pid_now(void) { return s_pid_ms; }

static void pid_setup(bb_fan_pid_t *pid, float *inp, float *out, float *sp,
                      bb_fan_pid_direction_t dir, bb_fan_pid_p_mode_t pon)
{
    s_pid_ms = 0;
    *inp = 50.0f;
    *out = 0.0f;
    *sp  = 60.0f;
    bb_fan_pid_init(pid, inp, out, sp, 5.0f, 0.1f, 2.0f, pon, dir);
    bb_fan_pid_set_sample_time(pid, 100); // 100 ms sample time for fast tests
    bb_fan_pid_set_output_limits(pid, 0.0f, 100.0f);
    bb_fan_pid_set_clock(pid, pid_now);
    // Seed lastTime so first compute fires immediately
    pid->lastTime = pid_now() - pid->sampleTime;
}

// ---------------------------------------------------------------------------
// set_clock: NULL fn arg is ignored (branch line 36)
// ---------------------------------------------------------------------------

void test_pid_set_clock_null_is_ignored(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    bb_fan_pid_init(&pid, &inp, &out, &sp, 5.0f, 0.1f, 2.0f,
                    BB_FAN_PID_P_ON_E, BB_FAN_PID_REVERSE);
    bb_fan_pid_now_ms_fn before = pid.now_ms;

    // Pass NULL — must not overwrite existing clock
    bb_fan_pid_set_clock(&pid, NULL);
    TEST_ASSERT_EQUAL_PTR(before, pid.now_ms);
}

// ---------------------------------------------------------------------------
// set_mode: AUTOMATIC when already AUTOMATIC — initialize() NOT called again
// (branch line 44: newAuto=true but pid->inAuto is already true)
// ---------------------------------------------------------------------------

void test_pid_set_mode_auto_when_already_auto_no_reinit(void)
{
    bb_fan_pid_t pid;
    float inp = 70.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    // Arm the PID
    bb_fan_pid_set_mode(&pid, BB_FAN_PID_AUTOMATIC);
    TEST_ASSERT_TRUE(pid.inAuto);
    float sum_after_first_arm = pid.outputSum;

    // Compute once so outputSum changes
    s_pid_ms += 200;
    bb_fan_pid_compute(&pid);
    float sum_mid = pid.outputSum;
    TEST_ASSERT_NOT_EQUAL(sum_after_first_arm, sum_mid);

    // Call set_mode(AUTOMATIC) again — should NOT call initialize() (which would reset outputSum)
    bb_fan_pid_set_mode(&pid, BB_FAN_PID_AUTOMATIC);
    TEST_ASSERT_TRUE(pid.inAuto);
    // outputSum must be unchanged (no re-init)
    TEST_ASSERT_EQUAL_FLOAT(sum_mid, pid.outputSum);
}

// ---------------------------------------------------------------------------
// compute: returns false when inAuto=false (branch line 52)
// ---------------------------------------------------------------------------

void test_pid_compute_returns_false_in_manual_mode(void)
{
    bb_fan_pid_t pid;
    float inp = 70.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    // Do NOT set mode to AUTOMATIC — inAuto stays false
    TEST_ASSERT_FALSE(pid.inAuto);

    s_pid_ms += 200; // time passes — but PID is in manual
    bool fired = bb_fan_pid_compute(&pid);
    TEST_ASSERT_FALSE(fired);
    // Output unchanged
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
}

// ---------------------------------------------------------------------------
// P_ON_M mode: outputSum -= kp*dInput; output = 0 + outputSum - kd*dInput
// (branches lines 63 and 68 — !pid->pOnE path)
// ---------------------------------------------------------------------------

void test_pid_p_on_m_mode_fires(void)
{
    bb_fan_pid_t pid;
    float inp = 70.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_M);

    bb_fan_pid_set_mode(&pid, BB_FAN_PID_AUTOMATIC);
    s_pid_ms += 200;
    bool fired = bb_fan_pid_compute(&pid);
    TEST_ASSERT_TRUE(fired);
    // Output must be in valid range (clamped 0..100)
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(0.0f, out);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(100.0f, out);
    // pOnE must be false
    TEST_ASSERT_FALSE(pid.pOnE);
}

// ---------------------------------------------------------------------------
// set_tunings_adv: negative Kp returns early without modifying (branch line 90)
// ---------------------------------------------------------------------------

void test_pid_set_tunings_negative_kp_is_rejected(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    float kp_before = pid.kp;
    float ki_before = pid.ki;
    float kd_before = pid.kd;

    bb_fan_pid_set_tunings_adv(&pid, -1.0f, 0.1f, 2.0f, BB_FAN_PID_P_ON_E);
    // Must be unchanged
    TEST_ASSERT_EQUAL_FLOAT(kp_before, pid.kp);
    TEST_ASSERT_EQUAL_FLOAT(ki_before, pid.ki);
    TEST_ASSERT_EQUAL_FLOAT(kd_before, pid.kd);
}

void test_pid_set_tunings_negative_ki_is_rejected(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    float kp_before = pid.kp;
    bb_fan_pid_set_tunings_adv(&pid, 5.0f, -0.1f, 2.0f, BB_FAN_PID_P_ON_E);
    TEST_ASSERT_EQUAL_FLOAT(kp_before, pid.kp);
}

void test_pid_set_tunings_negative_kd_is_rejected(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    float kp_before = pid.kp;
    bb_fan_pid_set_tunings_adv(&pid, 5.0f, 0.1f, -2.0f, BB_FAN_PID_P_ON_E);
    TEST_ASSERT_EQUAL_FLOAT(kp_before, pid.kp);
}

// ---------------------------------------------------------------------------
// set_tunings_adv: DIRECT direction does NOT negate gains (branch line 104)
// ---------------------------------------------------------------------------

void test_pid_set_tunings_direct_direction_positive_gains(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    // DIRECT direction: gains stay positive
    bb_fan_pid_init(&pid, &inp, &out, &sp, 5.0f, 0.1f, 2.0f,
                    BB_FAN_PID_P_ON_E, BB_FAN_PID_DIRECT);
    bb_fan_pid_set_sample_time(&pid, 100);
    bb_fan_pid_set_output_limits(&pid, 0.0f, 100.0f);

    // kp should be positive (DIRECT, not negated)
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, pid.kp);
}

// set_tunings (simple wrapper) — covers bb_fan_pid_set_tunings call
void test_pid_set_tunings_simple_wrapper(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    // Must not crash and must update dispKp
    bb_fan_pid_set_tunings(&pid, 3.0f, 0.05f, 1.0f);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, pid.dispKp);
}

// ---------------------------------------------------------------------------
// set_sample_time: zero/negative is a no-op (branch line 118)
// ---------------------------------------------------------------------------

void test_pid_set_sample_time_zero_is_noop(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    unsigned long st_before = pid.sampleTime;
    float ki_before = pid.ki;

    bb_fan_pid_set_sample_time(&pid, 0);
    TEST_ASSERT_EQUAL_UINT32(st_before, pid.sampleTime);
    TEST_ASSERT_EQUAL_FLOAT(ki_before, pid.ki);
}

void test_pid_set_sample_time_negative_is_noop(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    unsigned long st_before = pid.sampleTime;
    bb_fan_pid_set_sample_time(&pid, -10);
    TEST_ASSERT_EQUAL_UINT32(st_before, pid.sampleTime);
}

// ---------------------------------------------------------------------------
// set_output_limits: min >= max is a no-op (branch line 128)
// ---------------------------------------------------------------------------

void test_pid_set_output_limits_min_eq_max_noop(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    float min_before = pid.outMin;
    float max_before = pid.outMax;

    bb_fan_pid_set_output_limits(&pid, 50.0f, 50.0f); // min == max
    TEST_ASSERT_EQUAL_FLOAT(min_before, pid.outMin);
    TEST_ASSERT_EQUAL_FLOAT(max_before, pid.outMax);
}

void test_pid_set_output_limits_min_gt_max_noop(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);

    float min_before = pid.outMin;
    float max_before = pid.outMax;

    bb_fan_pid_set_output_limits(&pid, 80.0f, 20.0f); // min > max
    TEST_ASSERT_EQUAL_FLOAT(min_before, pid.outMin);
    TEST_ASSERT_EQUAL_FLOAT(max_before, pid.outMax);
}

// ---------------------------------------------------------------------------
// set_output_limits when inAuto=true: output > max clamped (branch line 133)
// and output < min clamped (branch line 134)
// ---------------------------------------------------------------------------

void test_pid_set_output_limits_clamps_output_above_max(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 80.0f, sp = 60.0f; // output starts at 80
    bb_fan_pid_init(&pid, &inp, &out, &sp, 5.0f, 0.1f, 2.0f,
                    BB_FAN_PID_P_ON_E, BB_FAN_PID_REVERSE);
    bb_fan_pid_set_clock(&pid, pid_now);
    bb_fan_pid_set_output_limits(&pid, 0.0f, 100.0f);
    bb_fan_pid_set_mode(&pid, BB_FAN_PID_AUTOMATIC);

    // Now shrink max below current output — output must be clamped
    bb_fan_pid_set_output_limits(&pid, 0.0f, 50.0f);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(50.0f, out);
}

void test_pid_set_output_limits_clamps_output_below_min(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 10.0f, sp = 60.0f; // output starts at 10
    bb_fan_pid_init(&pid, &inp, &out, &sp, 5.0f, 0.1f, 2.0f,
                    BB_FAN_PID_P_ON_E, BB_FAN_PID_REVERSE);
    bb_fan_pid_set_clock(&pid, pid_now);
    bb_fan_pid_set_output_limits(&pid, 0.0f, 100.0f);
    bb_fan_pid_set_mode(&pid, BB_FAN_PID_AUTOMATIC);

    // Raise min above current output — output must be clamped up
    bb_fan_pid_set_output_limits(&pid, 30.0f, 100.0f);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(30.0f, out);
}

// ---------------------------------------------------------------------------
// set_direction while inAuto=true and direction differs: gains negated
// (branch line 143)
// ---------------------------------------------------------------------------

void test_pid_set_direction_while_auto_negates_gains(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    // Start REVERSE
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);
    bb_fan_pid_set_mode(&pid, BB_FAN_PID_AUTOMATIC);
    TEST_ASSERT_TRUE(pid.inAuto);
    TEST_ASSERT_EQUAL_INT(BB_FAN_PID_REVERSE, pid.controllerDirection);

    float kp_reverse = pid.kp; // should be negative (REVERSE)

    // Change to DIRECT while armed — gains should flip sign
    bb_fan_pid_set_direction(&pid, BB_FAN_PID_DIRECT);
    TEST_ASSERT_EQUAL_INT(BB_FAN_PID_DIRECT, pid.controllerDirection);
    // kp should now be positive (negated again)
    TEST_ASSERT_EQUAL_FLOAT(-kp_reverse, pid.kp);
}

void test_pid_set_direction_same_dir_while_auto_no_flip(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    // Start REVERSE, arm
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);
    bb_fan_pid_set_mode(&pid, BB_FAN_PID_AUTOMATIC);
    TEST_ASSERT_TRUE(pid.inAuto);

    float kp_before = pid.kp;

    // Set same direction again — gains must NOT flip
    bb_fan_pid_set_direction(&pid, BB_FAN_PID_REVERSE);
    TEST_ASSERT_EQUAL_FLOAT(kp_before, pid.kp);
    TEST_ASSERT_EQUAL_INT(BB_FAN_PID_REVERSE, pid.controllerDirection);
}

// ---------------------------------------------------------------------------
// initialize: outputSum > outMax gets clamped (branch line 155)
// ---------------------------------------------------------------------------

void test_pid_initialize_clamps_output_sum_above_max(void)
{
    bb_fan_pid_t pid;
    // Start output at a high value so outputSum will be set high on initialize
    float inp = 50.0f, out = 200.0f, sp = 60.0f; // output=200 > outMax=100
    bb_fan_pid_init(&pid, &inp, &out, &sp, 5.0f, 0.1f, 2.0f,
                    BB_FAN_PID_P_ON_E, BB_FAN_PID_REVERSE);
    bb_fan_pid_set_output_limits(&pid, 0.0f, 100.0f);

    // initialize() copies output → outputSum; outputSum(200) > outMax(100)
    bb_fan_pid_initialize(&pid);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(100.0f, pid.outputSum);
}

// ---------------------------------------------------------------------------
// get_mode: returns AUTOMATIC / MANUAL (line 161, never called before)
// ---------------------------------------------------------------------------

void test_pid_get_mode_manual(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    bb_fan_pid_init(&pid, &inp, &out, &sp, 5.0f, 0.1f, 2.0f,
                    BB_FAN_PID_P_ON_E, BB_FAN_PID_REVERSE);
    TEST_ASSERT_EQUAL_INT(BB_FAN_PID_MANUAL, bb_fan_pid_get_mode(&pid));
}

void test_pid_get_mode_automatic(void)
{
    bb_fan_pid_t pid;
    float inp = 50.0f, out = 0.0f, sp = 60.0f;
    pid_setup(&pid, &inp, &out, &sp, BB_FAN_PID_REVERSE, BB_FAN_PID_P_ON_E);
    bb_fan_pid_set_mode(&pid, BB_FAN_PID_AUTOMATIC);
    TEST_ASSERT_EQUAL_INT(BB_FAN_PID_AUTOMATIC, bb_fan_pid_get_mode(&pid));
}

#endif /* CONFIG_BB_FAN_AUTOFAN */
