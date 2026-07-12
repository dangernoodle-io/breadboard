// Host tests for bb_fsm -- table-driven finite state machine primitive.
// No global/reset-hook state: bb_fsm is a pure per-instance library, so
// every test builds its own local `static const` table(s) plus a stack
// `bb_fsm_t` and context struct. Anonymized synthetic data throughout.
#include "unity.h"
#include "bb_fsm.h"

// ---------------------------------------------------------------------------
// Shared generic states/events for the small, focused scenarios (1-14).
// Independent bb_fsm_t instances per test -- reusing these ints across
// tests is safe, there is no shared state.
// ---------------------------------------------------------------------------
enum { S_A = 0, S_B = 1, S_C = 2 };
enum { E_GO = 0, E_BACK = 1, E_TICK = 2, E_KILL = 3, E_TIMEOUT = 4 };

typedef struct {
    char            trace[16];
    size_t          trace_len;
    int             action_count;
    void           *guard_data;
    bb_fsm_event_t  guard_event;
    void           *action_data;
    bb_fsm_event_t  action_event;
} trace_ctx_t;

static void trace_append(trace_ctx_t *tc, char c)
{
    if (tc->trace_len < sizeof(tc->trace) - 1) {
        tc->trace[tc->trace_len++] = c;
        tc->trace[tc->trace_len] = '\0';
    }
}

static void hook_entry_trace(bb_fsm_t *fsm, void *ctx, bb_fsm_state_t state)
{
    (void)fsm; (void)state;
    trace_append((trace_ctx_t *)ctx, 'E');
}

static void hook_exit_trace(bb_fsm_t *fsm, void *ctx, bb_fsm_state_t state)
{
    (void)fsm; (void)state;
    trace_append((trace_ctx_t *)ctx, 'X');
}

static void hook_entry_arm_timer(bb_fsm_t *fsm, void *ctx, bb_fsm_state_t state)
{
    (void)ctx; (void)state;
    bb_fsm_arm_timer(fsm, E_TIMEOUT, 100);
}

static void hook_entry_arm_timer_runtime(bb_fsm_t *fsm, void *ctx, bb_fsm_state_t state)
{
    (void)state;
    trace_ctx_t *tc = ctx;
    uint32_t ms = (uint32_t)(tc->action_count * 100 + 500);
    bb_fsm_arm_timer(fsm, E_TIMEOUT, ms);
}

static void action_trace(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm;
    trace_ctx_t *tc = ctx;
    trace_append(tc, 'A');
    tc->action_count++;
    tc->action_event = event;
    tc->action_data = evt_data;
}

static bool guard_false(void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)ctx; (void)event; (void)evt_data;
    return false;
}

static bool guard_true(void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)ctx; (void)event; (void)evt_data;
    return true;
}

static bool guard_capture(void *ctx, bb_fsm_event_t event, void *evt_data)
{
    trace_ctx_t *tc = ctx;
    tc->guard_event = event;
    tc->guard_data = evt_data;
    return true;
}

// ---------------------------------------------------------------------------
// 1. init sets initial + runs initial on_entry
// ---------------------------------------------------------------------------
void test_bb_fsm_init_sets_initial_and_runs_entry(void)
{
    static const bb_fsm_state_desc_t states[] = {
        { S_A, hook_entry_trace, NULL },
    };
    trace_ctx_t ctx = { 0 };
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0,
        .states = states, .state_count = 1,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL_INT(S_A, bb_fsm_state(&fsm));
    TEST_ASSERT_FALSE(bb_fsm_is_terminal(&fsm));
    TEST_ASSERT_EQUAL_STRING("E", ctx.trace);
}

// ---------------------------------------------------------------------------
// 2. transition A->B runs on_exit(A) before on_entry(B)
// ---------------------------------------------------------------------------
void test_bb_fsm_step_transition_runs_exit_before_entry(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO, NULL, action_trace, S_B },
    };
    static const bb_fsm_state_desc_t states[] = {
        { S_A, hook_entry_trace, hook_exit_trace },
        { S_B, hook_entry_trace, hook_exit_trace },
    };
    trace_ctx_t ctx = { 0 };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 1,
        .states = states, .state_count = 2,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_GO, NULL));
    TEST_ASSERT_EQUAL_INT(S_B, bb_fsm_state(&fsm));
    // init on_entry(A)="E"; step(): action="A" runs first, then on_exit(A)="X",
    // then on_entry(B)="E" -- exit(A) still precedes entry(B), just after the action.
    TEST_ASSERT_EQUAL_STRING("EAXE", ctx.trace);
}

// ---------------------------------------------------------------------------
// 3. no-match -> NOT_FOUND, state unchanged
// ---------------------------------------------------------------------------
void test_bb_fsm_step_no_match_returns_not_found_unchanged(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO, NULL, NULL, S_B },
    };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 1,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&fsm, E_BACK, NULL));
    TEST_ASSERT_EQUAL_INT(S_A, bb_fsm_state(&fsm));
}

// ---------------------------------------------------------------------------
// 4. guard-false falls through to the next row
// ---------------------------------------------------------------------------
void test_bb_fsm_step_guard_false_falls_through_to_next_row(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO, guard_false, action_trace, S_C },
        { S_A, E_GO, NULL,        action_trace, S_B },
    };
    trace_ctx_t ctx = { 0 };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 2,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_GO, NULL));
    TEST_ASSERT_EQUAL_INT(S_B, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(1, ctx.action_count);
}

// ---------------------------------------------------------------------------
// 5. guard-true first-match short-circuits
// ---------------------------------------------------------------------------
void test_bb_fsm_step_guard_true_first_match_short_circuits(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO, guard_true, action_trace, S_B },
        { S_A, E_GO, NULL,       action_trace, S_C },
    };
    trace_ctx_t ctx = { 0 };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 2,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_GO, NULL));
    TEST_ASSERT_EQUAL_INT(S_B, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(1, ctx.action_count); // row1 never evaluated
}

// ---------------------------------------------------------------------------
// 6. SAME: action fires, no exit/entry, timers untouched
// ---------------------------------------------------------------------------
void test_bb_fsm_step_same_runs_action_no_hooks_timers_kept(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_TICK, NULL, action_trace, BB_FSM_STATE_SAME },
    };
    static const bb_fsm_state_desc_t states[] = {
        { S_A, hook_entry_arm_timer, hook_exit_trace },
    };
    trace_ctx_t ctx = { 0 };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 1,
        .states = states, .state_count = 1,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_TICK, NULL));
    TEST_ASSERT_EQUAL_INT(S_A, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(1, ctx.action_count);
    TEST_ASSERT_EQUAL_STRING("A", ctx.trace); // no additional E/X beyond init's entry (which armed, no trace char)
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));

    bb_fsm_event_t ev;
    uint32_t ms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&fsm, 0, &ev, &ms));
    TEST_ASSERT_EQUAL_INT(E_TIMEOUT, ev);
    TEST_ASSERT_EQUAL_UINT32(100, ms);
}

// ---------------------------------------------------------------------------
// 7. TERMINAL: action fires, is_terminal true, next step -> INVALID_STATE
// ---------------------------------------------------------------------------
void test_bb_fsm_step_terminal_latches_and_rejects_further_steps(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_KILL, NULL, action_trace, BB_FSM_STATE_TERMINAL },
    };
    trace_ctx_t ctx = { 0 };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 1,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_KILL, NULL));
    TEST_ASSERT_TRUE(bb_fsm_is_terminal(&fsm));
    TEST_ASSERT_EQUAL_INT(1, ctx.action_count);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_fsm_step(&fsm, E_KILL, NULL));
    TEST_ASSERT_EQUAL_INT(1, ctx.action_count); // no-op, action did not re-fire
}

// ---------------------------------------------------------------------------
// 8. arm+timer_at: entry arms a runtime-computed ms, timer_at reports it,
//    feeding timeout_event back to step() fires the row.
// ---------------------------------------------------------------------------
void test_bb_fsm_arm_timer_entry_arms_and_timer_at_reports_then_fires(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO,      NULL, NULL,         S_B },
        { S_B, E_TIMEOUT, NULL, action_trace, S_C },
    };
    static const bb_fsm_state_desc_t states[] = {
        { S_B, hook_entry_arm_timer_runtime, NULL },
    };
    trace_ctx_t ctx = { 0 };
    ctx.action_count = 3; // pre-seed so the "runtime-computed" ms is non-default
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 2,
        .states = states, .state_count = 1,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_GO, NULL));
    TEST_ASSERT_EQUAL_INT(S_B, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));

    bb_fsm_event_t ev;
    uint32_t ms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&fsm, 0, &ev, &ms));
    TEST_ASSERT_EQUAL_INT(E_TIMEOUT, ev);
    TEST_ASSERT_EQUAL_UINT32(800, ms); // 3*100 + 500

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(S_C, bb_fsm_state(&fsm));
}

// ---------------------------------------------------------------------------
// 9. arm overflow + multi-slot proof. Native host build overrides
//    CONFIG_BB_FSM_MAX_TIMERS=2 (see platformio.ini) so this file can prove
//    the seam is genuinely N-ready, not hardcoded to a single scalar --
//    the shipped Kconfig default stays 1.
// ---------------------------------------------------------------------------
void test_bb_fsm_arm_timer_two_distinct_slots_ok(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TICK, 100));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_KILL, 200));
    TEST_ASSERT_EQUAL(2, (int)bb_fsm_timer_count(&fsm));

    bb_fsm_event_t ev;
    uint32_t ms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&fsm, 0, &ev, &ms));
    TEST_ASSERT_EQUAL_INT(E_TICK, ev);
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&fsm, 1, &ev, &ms)); // skips slot 0 (seen != i) before matching
    TEST_ASSERT_EQUAL_INT(E_KILL, ev);
}

void test_bb_fsm_arm_timer_third_distinct_no_space(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TICK, 100));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_KILL, 200));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_fsm_arm_timer(&fsm, E_TIMEOUT, 50));
    TEST_ASSERT_EQUAL(2, (int)bb_fsm_timer_count(&fsm));
}

void test_bb_fsm_disarm_frees_slot_for_new_arm(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TICK, 100));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_KILL, 200));
    bb_fsm_disarm_timer(&fsm, E_TICK);
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TIMEOUT, 50));
    TEST_ASSERT_EQUAL(2, (int)bb_fsm_timer_count(&fsm));
}

// ---------------------------------------------------------------------------
// 10. disarm
// ---------------------------------------------------------------------------
void test_bb_fsm_disarm_timer_removes_armed_event(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TICK, 100));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));
    bb_fsm_disarm_timer(&fsm, E_TICK);
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm));
    bb_fsm_event_t ev;
    uint32_t ms;
    TEST_ASSERT_FALSE(bb_fsm_timer_at(&fsm, 0, &ev, &ms));
}

// ---------------------------------------------------------------------------
// 11. timers cleared on a real transition, kept on SAME
// ---------------------------------------------------------------------------
void test_bb_fsm_timers_cleared_on_real_transition(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO, NULL, NULL, S_B },
    };
    static const bb_fsm_state_desc_t states[] = {
        { S_A, hook_entry_arm_timer, NULL },
    };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 1,
        .states = states, .state_count = 1,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_GO, NULL));
    TEST_ASSERT_EQUAL_INT(S_B, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm)); // B has no on_entry -- cleared, not re-armed
}

// ---------------------------------------------------------------------------
// 11b. TERMINAL clears an armed timer (MED-1): a state that arms a timeout
// in its on_entry, then transitions to TERMINAL, must not leave the timer
// armed -- otherwise the shell re-arms a real OS timer that fires into a
// now-rejected step forever.
// ---------------------------------------------------------------------------
void test_bb_fsm_step_terminal_clears_armed_timer(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_KILL, NULL, NULL, BB_FSM_STATE_TERMINAL },
    };
    static const bb_fsm_state_desc_t states[] = {
        { S_A, hook_entry_arm_timer, NULL },
    };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 1,
        .states = states, .state_count = 1,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm)); // init's on_entry armed it

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_KILL, NULL));
    TEST_ASSERT_TRUE(bb_fsm_is_terminal(&fsm));
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm)); // latch must not leak the armed timer
}

// ---------------------------------------------------------------------------
// 11c. Contract-locking (nit-11): an action arms a timer on a row whose
// `next` is a DIFFERENT concrete state -- the arm is dropped by the
// post-action clear, not kept. Locks the exact seam MED-2/the header docs
// describe.
// ---------------------------------------------------------------------------
static void action_arm_then_go(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)ctx; (void)event; (void)evt_data;
    bb_fsm_arm_timer(fsm, E_TIMEOUT, 100);
}

void test_bb_fsm_step_action_arm_on_concrete_transition_dropped(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO, NULL, action_arm_then_go, S_B },
    };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 1,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_GO, NULL));
    TEST_ASSERT_EQUAL_INT(S_B, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm)); // action's arm was dropped by the post-action clear
}

// ---------------------------------------------------------------------------
// 12. ANY wildcard matches from >=2 distinct states
// ---------------------------------------------------------------------------
void test_bb_fsm_any_wildcard_matches_from_multiple_states(void)
{
    static const bb_fsm_row_t rows[] = {
        { BB_FSM_STATE_ANY, E_GO, NULL, NULL, S_C },
    };
    bb_fsm_desc_t desc_a = {
        .rows = rows, .row_count = 1,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm_a;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm_a, &desc_a));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm_a, E_GO, NULL));
    TEST_ASSERT_EQUAL_INT(S_C, bb_fsm_state(&fsm_a));

    bb_fsm_desc_t desc_b = {
        .rows = rows, .row_count = 1,
        .states = NULL, .state_count = 0,
        .initial = S_B, .ctx = NULL,
    };
    bb_fsm_t fsm_b;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm_b, &desc_b));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm_b, E_GO, NULL));
    TEST_ASSERT_EQUAL_INT(S_C, bb_fsm_state(&fsm_b));
}

// ---------------------------------------------------------------------------
// 13. evt_data threaded unchanged to guard AND action
// ---------------------------------------------------------------------------
void test_bb_fsm_evt_data_threaded_to_guard_and_action(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO, guard_capture, action_trace, S_B },
    };
    trace_ctx_t ctx = { 0 };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 1,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    int payload = 42;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_GO, &payload));
    TEST_ASSERT_EQUAL_PTR(&payload, ctx.guard_data);
    TEST_ASSERT_EQUAL_PTR(&payload, ctx.action_data);
    TEST_ASSERT_EQUAL_INT(E_GO, ctx.guard_event);
    TEST_ASSERT_EQUAL_INT(E_GO, ctx.action_event);
}

// ---------------------------------------------------------------------------
// 14. NULL fsm/desc -> INVALID_ARG (+ malformed desc)
// ---------------------------------------------------------------------------
void test_bb_fsm_null_and_malformed_args_invalid_arg(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0,
        .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fsm_init(NULL, &desc));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fsm_init(&fsm, NULL));

    bb_fsm_desc_t bad_rows = desc;
    bad_rows.rows = NULL;
    bad_rows.row_count = 1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fsm_init(&fsm, &bad_rows));

    bb_fsm_desc_t bad_states = desc;
    bad_states.states = NULL;
    bad_states.state_count = 1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fsm_init(&fsm, &bad_states));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fsm_step(NULL, E_GO, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fsm_arm_timer(NULL, E_GO, 10));
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(NULL));
    bb_fsm_event_t ev;
    uint32_t ms;
    TEST_ASSERT_FALSE(bb_fsm_timer_at(NULL, 0, &ev, &ms));
    bb_fsm_disarm_timer(NULL, E_GO); // must not crash
}

// ---------------------------------------------------------------------------
// 14b. LOW-6: calling bb_fsm_step()/bb_fsm_arm_timer() on an instance never
// passed to bb_fsm_init() is rejected (INVALID_STATE), not a NULL deref
// via fsm->desc.
// ---------------------------------------------------------------------------
void test_bb_fsm_step_uninitialized_instance_invalid_state(void)
{
    bb_fsm_t fsm; // deliberately never bb_fsm_init()'d
    fsm.desc = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_fsm_step(&fsm, E_GO, NULL));
}

void test_bb_fsm_arm_timer_uninitialized_instance_invalid_state(void)
{
    bb_fsm_t fsm; // deliberately never bb_fsm_init()'d
    fsm.desc = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_fsm_arm_timer(&fsm, E_GO, 10));
}

// ---------------------------------------------------------------------------
// Branch-coverage rounding out: NULL-fsm accessors, hook-missing halves of
// run_entry/run_exit, arm_timer's ms==0 (disarm-via-rearm and no-op-miss)
// paths, disarm_timer's no-match path, timer_at's NULL out-param halves.
// ---------------------------------------------------------------------------
void test_bb_fsm_state_and_is_terminal_null_fsm_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(BB_FSM_STATE_ANY, bb_fsm_state(NULL));
    TEST_ASSERT_FALSE(bb_fsm_is_terminal(NULL));
}

void test_bb_fsm_run_entry_exit_missing_hook_halves_no_crash(void)
{
    static const bb_fsm_row_t rows[] = {
        { S_A, E_GO,   NULL, NULL, S_B },
        { S_B, E_BACK, NULL, NULL, S_A },
    };
    static const bb_fsm_state_desc_t states[] = {
        { S_A, NULL,             hook_exit_trace }, // no on_entry, has on_exit
        { S_B, hook_entry_trace, NULL },             // has on_entry, no on_exit
    };
    trace_ctx_t ctx = { 0 };
    bb_fsm_desc_t desc = {
        .rows = rows, .row_count = 2,
        .states = states, .state_count = 2,
        .initial = S_A, .ctx = &ctx,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc)); // run_entry(A): sd found, on_entry NULL
    TEST_ASSERT_EQUAL_STRING("", ctx.trace);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_GO, NULL)); // exit(A) fires, entry(B) fires
    TEST_ASSERT_EQUAL_STRING("XE", ctx.trace);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, E_BACK, NULL)); // exit(B): sd found, on_exit NULL
    TEST_ASSERT_EQUAL_STRING("XE", ctx.trace); // unchanged -- no exit(B)/entry(A) trace chars
    TEST_ASSERT_EQUAL_INT(S_A, bb_fsm_state(&fsm));
}

void test_bb_fsm_arm_timer_zero_ms_on_existing_slot_disarms(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0, .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TICK, 100));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TICK, 0)); // re-arm with ms==0 disarms
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm));
}

void test_bb_fsm_arm_timer_zero_ms_no_matching_slot_is_noop(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0, .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_KILL, 0)); // nothing armed under E_KILL -- no-op
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm));
}

void test_bb_fsm_disarm_timer_no_match_is_noop(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0, .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TICK, 100));
    bb_fsm_disarm_timer(&fsm, E_KILL); // armed set is non-empty but has no E_KILL slot
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));
}

void test_bb_fsm_timer_at_null_out_params_are_optional(void)
{
    bb_fsm_desc_t desc = {
        .rows = NULL, .row_count = 0, .states = NULL, .state_count = 0,
        .initial = S_A, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_arm_timer(&fsm, E_TICK, 100));

    uint32_t ms = 0;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&fsm, 0, NULL, &ms));
    TEST_ASSERT_EQUAL_UINT32(100, ms);

    bb_fsm_event_t ev = BB_FSM_EVENT_NONE;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&fsm, 0, &ev, NULL));
    TEST_ASSERT_EQUAL_INT(E_TICK, ev);
}

// ===========================================================================
// 15. REALISM: wifi reconnect-policy replica
// ===========================================================================
enum { WIFI_CONNECTING = 0, WIFI_IDLE = 1, WIFI_BACKOFF = 2 };
enum { WEVT_GOT_IP = 0, WEVT_DISCONNECT = 1, WEVT_RECOVERY = 2, WEVT_TIMEOUT = 3 };

typedef struct {
    int  fail_count;
    int  max_fail;
    bool should_backoff;
    uint32_t last_backoff_ms;
    int  recovery_calls;
    int  reconnect_calls;
} wifi_ctx_t;

static bool wifi_guard_persistent_fail(void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    wifi_ctx_t *w = ctx;
    return w->fail_count >= w->max_fail;
}

static bool wifi_guard_should_backoff(void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    wifi_ctx_t *w = ctx;
    return w->should_backoff;
}

static void wifi_action_connected(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    ((wifi_ctx_t *)ctx)->fail_count = 0;
}

static void wifi_action_persistent_fail(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    ((wifi_ctx_t *)ctx)->fail_count++;
}

static void wifi_action_compute_backoff(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_ctx_t *w = ctx;
    w->fail_count++;
    w->last_backoff_ms = (uint32_t)(100 * w->fail_count); // runtime-computed duration
}

static void wifi_action_immediate_reconnect(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    ((wifi_ctx_t *)ctx)->reconnect_calls++;
}

static void wifi_action_recovery(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    ((wifi_ctx_t *)ctx)->recovery_calls++;
}

// Entry hook for BACKOFF -- arms the timer AFTER the transition-triggered
// clear, using the duration the DISCONNECT action just computed into ctx.
static void wifi_backoff_on_entry(bb_fsm_t *fsm, void *ctx, bb_fsm_state_t state)
{
    (void)state;
    wifi_ctx_t *w = ctx;
    bb_fsm_arm_timer(fsm, WEVT_TIMEOUT, w->last_backoff_ms);
}

// Table order matters: persistent-fail is checked before should-backoff,
// which is checked before the unconditional immediate-reconnect fallback.
static const bb_fsm_row_t wifi_rows[] = {
    { WIFI_CONNECTING, WEVT_GOT_IP,     NULL,                       wifi_action_connected,          WIFI_IDLE },
    { WIFI_CONNECTING, WEVT_DISCONNECT, wifi_guard_persistent_fail, wifi_action_persistent_fail,     BB_FSM_STATE_TERMINAL },
    { WIFI_CONNECTING, WEVT_DISCONNECT, wifi_guard_should_backoff,  wifi_action_compute_backoff,     WIFI_BACKOFF },
    { WIFI_CONNECTING, WEVT_DISCONNECT, NULL,                       wifi_action_immediate_reconnect, WIFI_CONNECTING },
    { WIFI_BACKOFF,     WEVT_RECOVERY,  NULL,                       wifi_action_recovery,            BB_FSM_STATE_SAME },
    { WIFI_BACKOFF,     WEVT_TIMEOUT,   NULL,                       NULL,                            WIFI_CONNECTING },
};

static const bb_fsm_state_desc_t wifi_states[] = {
    { WIFI_BACKOFF, wifi_backoff_on_entry, NULL },
};

static void wifi_desc_init(bb_fsm_desc_t *desc, wifi_ctx_t *ctx)
{
    desc->rows = wifi_rows;
    desc->row_count = sizeof(wifi_rows) / sizeof(wifi_rows[0]);
    desc->states = wifi_states;
    desc->state_count = sizeof(wifi_states) / sizeof(wifi_states[0]);
    desc->initial = WIFI_CONNECTING;
    desc->ctx = ctx;
}

void test_bb_fsm_wifi_replica_persistent_fail_terminal(void)
{
    wifi_ctx_t ctx = { .fail_count = 1, .max_fail = 1, .should_backoff = true };
    bb_fsm_desc_t desc;
    wifi_desc_init(&desc, &ctx);
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, WEVT_DISCONNECT, NULL));
    TEST_ASSERT_TRUE(bb_fsm_is_terminal(&fsm));
    TEST_ASSERT_EQUAL_INT(2, ctx.fail_count); // persistent-fail row wins despite should_backoff also true
}

void test_bb_fsm_wifi_replica_backoff_then_timeout_reconnects(void)
{
    wifi_ctx_t ctx = { .fail_count = 0, .max_fail = 5, .should_backoff = true };
    bb_fsm_desc_t desc;
    wifi_desc_init(&desc, &ctx);
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, WEVT_DISCONNECT, NULL));
    TEST_ASSERT_EQUAL_INT(WIFI_BACKOFF, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));
    bb_fsm_event_t ev;
    uint32_t ms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&fsm, 0, &ev, &ms));
    TEST_ASSERT_EQUAL_INT(WEVT_TIMEOUT, ev);
    TEST_ASSERT_EQUAL_UINT32(100, ms); // fail_count became 1

    // RECOVERY while backed off is a SAME-with-action transition
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, WEVT_RECOVERY, NULL));
    TEST_ASSERT_EQUAL_INT(WIFI_BACKOFF, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(1, ctx.recovery_calls);
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm)); // SAME keeps the armed timer

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, WEVT_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WIFI_CONNECTING, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm)); // real transition clears
}

void test_bb_fsm_wifi_replica_immediate_reconnect_and_got_ip(void)
{
    wifi_ctx_t ctx = { .fail_count = 0, .max_fail = 5, .should_backoff = false };
    bb_fsm_desc_t desc;
    wifi_desc_init(&desc, &ctx);
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, WEVT_DISCONNECT, NULL));
    TEST_ASSERT_EQUAL_INT(WIFI_CONNECTING, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(1, ctx.reconnect_calls);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, WEVT_GOT_IP, NULL));
    TEST_ASSERT_EQUAL_INT(WIFI_IDLE, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(0, ctx.fail_count);
}

// ===========================================================================
// 16. REALISM: ota_pull replica
// ===========================================================================
enum { OTA_DOWNLOADING = 0, OTA_VERIFYING = 1, OTA_ERROR = 2 };
enum { OEVT_CHUNK_FAIL = 0, OEVT_ALL_CHUNKS_OK = 1, OEVT_VERIFY_OK = 2, OEVT_RETRY_TIMEOUT = 3 };

typedef struct {
    bool     deterministic_abort;
    int      retry_count;
    int      abort_count;
} ota_ctx_t;

static bool ota_guard_deterministic_abort(void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    return ((ota_ctx_t *)ctx)->deterministic_abort;
}

static void ota_action_abort(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    ((ota_ctx_t *)ctx)->abort_count++;
}

// SAME row -- no clear happens, so it is safe for the action itself to arm
// the retry timer directly (no shell-facing entry hook needed here).
static void ota_action_retry_with_backoff(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    ota_ctx_t *o = ctx;
    o->retry_count++;
    bb_fsm_arm_timer(fsm, OEVT_RETRY_TIMEOUT, (uint32_t)(50 * o->retry_count));
}

static const bb_fsm_row_t ota_rows[] = {
    { OTA_DOWNLOADING, OEVT_CHUNK_FAIL,     ota_guard_deterministic_abort, ota_action_abort,             OTA_ERROR },
    { OTA_DOWNLOADING, OEVT_CHUNK_FAIL,     NULL,                          ota_action_retry_with_backoff, BB_FSM_STATE_SAME },
    { OTA_DOWNLOADING, OEVT_ALL_CHUNKS_OK,  NULL,                          NULL,                          OTA_VERIFYING },
    { OTA_VERIFYING,   OEVT_VERIFY_OK,      NULL,                          NULL,                          BB_FSM_STATE_TERMINAL },
};

static void ota_desc_init(bb_fsm_desc_t *desc, ota_ctx_t *ctx, bb_fsm_state_t initial)
{
    desc->rows = ota_rows;
    desc->row_count = sizeof(ota_rows) / sizeof(ota_rows[0]);
    desc->states = NULL;
    desc->state_count = 0;
    desc->initial = initial;
    desc->ctx = ctx;
}

void test_bb_fsm_ota_pull_replica_self_loop_retry_then_deterministic_abort(void)
{
    ota_ctx_t ctx = { .deterministic_abort = false, .retry_count = 0, .abort_count = 0 };
    bb_fsm_desc_t desc;
    ota_desc_init(&desc, &ctx, OTA_DOWNLOADING);
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, OEVT_CHUNK_FAIL, NULL));
    TEST_ASSERT_EQUAL_INT(OTA_DOWNLOADING, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(1, ctx.retry_count);
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, OEVT_CHUNK_FAIL, NULL));
    TEST_ASSERT_EQUAL_INT(OTA_DOWNLOADING, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(2, ctx.retry_count);
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm)); // re-arm overwrites, still 1 slot

    ctx.deterministic_abort = true;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, OEVT_CHUNK_FAIL, NULL));
    TEST_ASSERT_EQUAL_INT(OTA_ERROR, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL_INT(1, ctx.abort_count);
    TEST_ASSERT_EQUAL(0, (int)bb_fsm_timer_count(&fsm)); // real transition clears
}

void test_bb_fsm_ota_pull_replica_verifying_to_complete_terminal(void)
{
    ota_ctx_t ctx = { .deterministic_abort = false, .retry_count = 0, .abort_count = 0 };
    bb_fsm_desc_t desc;
    ota_desc_init(&desc, &ctx, OTA_VERIFYING);
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, OEVT_VERIFY_OK, NULL));
    TEST_ASSERT_TRUE(bb_fsm_is_terminal(&fsm));
}

// ===========================================================================
// 17. REALISM: button replica
// ===========================================================================
enum { BTN_IDLE = 0, BTN_PRESSED = 1, BTN_WAIT_DOUBLE = 2 };
enum { BEVT_TICK = 0, BEVT_DOWN = 1, BEVT_UP = 2 };

static void btn_entry_rearm_tick(bb_fsm_t *fsm, void *ctx, bb_fsm_state_t state)
{
    (void)ctx; (void)state;
    bb_fsm_arm_timer(fsm, BEVT_TICK, 20); // periodic tick, re-armed on every entry
}

static const bb_fsm_row_t btn_rows[] = {
    { BTN_IDLE,    BEVT_DOWN, NULL, NULL, BTN_PRESSED },
    { BTN_PRESSED, BEVT_UP,   NULL, NULL, BTN_WAIT_DOUBLE },
};

static const bb_fsm_state_desc_t btn_states[] = {
    { BTN_IDLE,        btn_entry_rearm_tick, NULL },
    { BTN_PRESSED,     btn_entry_rearm_tick, NULL },
    { BTN_WAIT_DOUBLE, btn_entry_rearm_tick, NULL },
};

void test_bb_fsm_button_replica_periodic_tick_rearmed_across_edges(void)
{
    bb_fsm_desc_t desc = {
        .rows = btn_rows, .row_count = sizeof(btn_rows) / sizeof(btn_rows[0]),
        .states = btn_states, .state_count = sizeof(btn_states) / sizeof(btn_states[0]),
        .initial = BTN_IDLE, .ctx = NULL,
    };
    bb_fsm_t fsm;
    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_init(&fsm, &desc));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm));
    bb_fsm_event_t ev;
    uint32_t ms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&fsm, 0, &ev, &ms));
    TEST_ASSERT_EQUAL_INT(BEVT_TICK, ev);
    TEST_ASSERT_EQUAL_UINT32(20, ms);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, BEVT_DOWN, NULL));
    TEST_ASSERT_EQUAL_INT(BTN_PRESSED, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm)); // re-armed by PRESSED's on_entry

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&fsm, BEVT_UP, NULL));
    TEST_ASSERT_EQUAL_INT(BTN_WAIT_DOUBLE, bb_fsm_state(&fsm));
    TEST_ASSERT_EQUAL(1, (int)bb_fsm_timer_count(&fsm)); // re-armed by WAIT_DOUBLE's on_entry
}
