#include "unity.h"
#include "wifi_prov_policy.h"

#include <string.h>

// ===========================================================================
// B1-809 PR2/PR3a -- pure provisioning-orchestrator policy tests. Drives the
// REAL FSM table (wifi_prov_fsm_init) via a fully-faked adapter recording
// every side-effecting call, its order, and every log emission -- both call
// order and log-evt identity are asserted, not just return codes.
// ===========================================================================

// Arbitrary non-zero settle duration -- tests assert the FSM stashes/arms
// this exact value, never that any particular number is "correct" (that's a
// Kconfig/shell concern, PR3b).
#define TEST_SETTLE_MS 250u

#define LOG_HIST_MAX 8

typedef struct {
    bb_err_t ap_start_result;
    bb_err_t prov_start_result;
    int      ap_start_calls;
    int      ap_stop_calls;
    int      prov_start_calls;
    int      prov_stop_calls;
    int      creds_notify_calls;
    int      log_calls;
    wifi_prov_log_evt_t log_hist[LOG_HIST_MAX];
    int      log_hist_n;
    // Call-order tracking (act_close_portal's exact-order contract).
    int      call_seq;
    int      prov_stop_seq;
    int      ap_stop_seq;
    int      creds_notify_seq;
} fake_t;

static fake_t s_fake;

static bb_err_t fake_ap_start(void)
{
    s_fake.ap_start_calls++;
    return s_fake.ap_start_result;
}

static void fake_ap_stop(void)
{
    s_fake.ap_stop_calls++;
    s_fake.ap_stop_seq = ++s_fake.call_seq;
}

static bb_err_t fake_prov_start(void)
{
    s_fake.prov_start_calls++;
    return s_fake.prov_start_result;
}

static void fake_prov_stop(void)
{
    s_fake.prov_stop_calls++;
    s_fake.prov_stop_seq = ++s_fake.call_seq;
}

static void fake_creds_notify(void)
{
    s_fake.creds_notify_calls++;
    s_fake.creds_notify_seq = ++s_fake.call_seq;
}

static void fake_log(wifi_prov_log_evt_t evt)
{
    s_fake.log_calls++;
    if (s_fake.log_hist_n < LOG_HIST_MAX) {
        s_fake.log_hist[s_fake.log_hist_n++] = evt;
    }
}

static const wifi_prov_adapter_t s_adapter = {
    .ap_start_fn             = fake_ap_start,
    .ap_stop_fn               = fake_ap_stop,
    .prov_start_fn            = fake_prov_start,
    .prov_stop_fn              = fake_prov_stop,
    .creds_arrived_notify_fn = fake_creds_notify,
    .log_fn                   = fake_log,
};

// Fresh fixture: fake reset (default: both starts succeed -- the common
// case; individual tests flip a result to exercise a failure branch), ctx
// zeroed, FSM initialized at `initial`.
static void fixture_init(wifi_prov_ctx_t *ctx, bb_fsm_state_t initial)
{
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.ap_start_result = BB_OK;
    s_fake.prov_start_result = BB_OK;

    memset(ctx, 0, sizeof(*ctx));
    ctx->adapter = &s_adapter;
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_fsm_init(ctx, initial, TEST_SETTLE_MS));
}

// --- Rows 1/4/5: EV_ENTRY_FIRST_BOOT happy path, WP_IDLE -> WP_ACTIVE ---

void test_wifi_prov_first_boot_reaches_active(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_FIRST_BOOT);

    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(1, s_fake.prov_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(2, s_fake.log_calls);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_ENTRY_FIRST_BOOT, s_fake.log_hist[0]);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_PORTAL_OPEN, s_fake.log_hist[1]);
}

// --- Rows 2/4/5: EV_ENTRY_DEPROV_ANOMALY happy path ---

void test_wifi_prov_deprov_anomaly_reaches_active(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_DEPROV_ANOMALY);

    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(1, s_fake.prov_start_calls);
    TEST_ASSERT_EQUAL(2, s_fake.log_calls);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_ENTRY_DEPROV_ANOMALY, s_fake.log_hist[0]);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_PORTAL_OPEN, s_fake.log_hist[1]);
}

// --- Rows 3/4/5: EV_ENTRY_USER_REQUESTED happy path (from WP_IDLE) ---

void test_wifi_prov_user_requested_reaches_active(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_USER_REQUESTED);

    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(1, s_fake.prov_start_calls);
    TEST_ASSERT_EQUAL(2, s_fake.log_calls);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_ENTRY_USER_REQUESTED, s_fake.log_hist[0]);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_PORTAL_OPEN, s_fake.log_hist[1]);
}

// --- ap_start failure: stays WP_IDLE, prov_start NEVER called, correct log
// evt, ap_stop NOT called (nothing to roll back) ---

void test_wifi_prov_ap_start_failure_stays_idle_no_prov_start(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);
    s_fake.ap_start_result = BB_ERR_INVALID_STATE;

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_FIRST_BOOT);

    TEST_ASSERT_EQUAL_INT(WP_IDLE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.prov_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(2, s_fake.log_calls);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_ENTRY_FIRST_BOOT, s_fake.log_hist[0]);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_AP_START_FAILED, s_fake.log_hist[1]);
}

// --- prov_start failure after ap_start ok: stays WP_IDLE, ap_stop IS
// called (rollback), correct log evt ---

void test_wifi_prov_prov_start_failure_rolls_back_ap(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);
    s_fake.prov_start_result = BB_ERR_INVALID_STATE;

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_USER_REQUESTED);

    TEST_ASSERT_EQUAL_INT(WP_IDLE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(1, s_fake.prov_start_calls);
    TEST_ASSERT_EQUAL(1, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(2, s_fake.log_calls);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_ENTRY_USER_REQUESTED, s_fake.log_hist[0]);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_PROV_START_FAILED, s_fake.log_hist[1]);
}

// --- re-entry while WP_ACTIVE: no AP/prov restart, correct log evt, still
// WP_ACTIVE ---

void test_wifi_prov_reentry_while_active_is_noop(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_ACTIVE);

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_USER_REQUESTED);

    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.prov_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(0, s_fake.prov_stop_calls);
    // WP_ACTIVE's on_entry already fired once at fixture_init() time
    // (portal-open) -- the re-entry itself contributes exactly one more
    // log call (entry-already-active).
    TEST_ASSERT_EQUAL(2, s_fake.log_calls);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_PORTAL_OPEN, s_fake.log_hist[0]);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_ENTRY_ALREADY_ACTIVE, s_fake.log_hist[1]);
}

// --- EV_SAVE_SUCCESS: moves to WP_CLOSING WITHOUT tearing anything down
// yet -- no adapter calls, timer armed with ctx's settle_ms ---

void test_wifi_prov_save_success_enters_closing_without_teardown(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_ACTIVE);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_SAVE_SUCCESS, NULL));

    TEST_ASSERT_EQUAL_INT(WP_CLOSING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.prov_stop_calls);
    TEST_ASSERT_EQUAL(0, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(0, s_fake.creds_notify_calls);

    TEST_ASSERT_EQUAL(1, bb_fsm_timer_count(&ctx.fsm));
    bb_fsm_event_t armed_evt;
    uint32_t armed_ms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &armed_evt, &armed_ms));
    TEST_ASSERT_EQUAL(EV_SETTLE_TIMEOUT, armed_evt);
    TEST_ASSERT_EQUAL_UINT32(TEST_SETTLE_MS, armed_ms);
}

// --- Full happy path IDLE -> ACTIVE -> (save) -> CLOSING -> (timeout) ->
// IDLE: exact call order prov_stop -> ap_stop -> creds_arrived_notify, log
// evt WIFI_PROV_LOG_PORTAL_CLOSED fires, timer is gone once back in IDLE ---

void test_wifi_prov_settle_timeout_closes_portal_in_order(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_FIRST_BOOT);
    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_SAVE_SUCCESS, NULL));
    TEST_ASSERT_EQUAL_INT(WP_CLOSING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, bb_fsm_timer_count(&ctx.fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_SETTLE_TIMEOUT, NULL));

    TEST_ASSERT_EQUAL_INT(WP_IDLE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, bb_fsm_timer_count(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.prov_stop_calls);
    TEST_ASSERT_EQUAL(1, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(1, s_fake.creds_notify_calls);
    TEST_ASSERT_EQUAL(1, s_fake.prov_stop_seq);
    TEST_ASSERT_EQUAL(2, s_fake.ap_stop_seq);
    TEST_ASSERT_EQUAL(3, s_fake.creds_notify_seq);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_PORTAL_CLOSED, s_fake.log_hist[s_fake.log_hist_n - 1]);
}

// --- Deliberate no-op cells: WP_CLOSING + EV_SAVE_SUCCESS -- repeated POST
// /save while already closing is idempotent (no row, no state change, no
// adapter calls) ---

void test_wifi_prov_closing_save_success_is_noop(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_CLOSING);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_SAVE_SUCCESS, NULL));

    TEST_ASSERT_EQUAL_INT(WP_CLOSING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.prov_stop_calls);
    TEST_ASSERT_EQUAL(0, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(0, s_fake.creds_notify_calls);
}

// --- Deliberate no-op cells: WP_CLOSING + EV_ENTRY_USER_REQUESTED -- a
// re-provision request during the close window is dropped, not queued; the
// user can re-request once back in WP_IDLE. drive_entry's hop-1 `!= BB_OK`
// return already handles this safely (asserted via the driver, not a bare
// bb_fsm_step, to pin that safety net too). ---

void test_wifi_prov_closing_entry_user_requested_is_noop(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_CLOSING);

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_USER_REQUESTED);

    TEST_ASSERT_EQUAL_INT(WP_CLOSING, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.prov_start_calls);
}

// --- Deliberate no-op cells: EV_SETTLE_TIMEOUT from WP_IDLE/WP_ACTIVE -- the
// timer is only ever armed entering WP_CLOSING, so a stray/late timeout
// event from either other state is a harmless no-op. ---

void test_wifi_prov_idle_settle_timeout_is_noop(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_SETTLE_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WP_IDLE, bb_fsm_state(&ctx.fsm));
}

void test_wifi_prov_active_settle_timeout_is_noop(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_ACTIVE);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_SETTLE_TIMEOUT, NULL));
    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));
}

// --- Unmatched state/event pairs are a harmless no-op: BB_ERR_NOT_FOUND,
// no state mutation, no side effects (bb_fsm_step's documented contract --
// see components/bb_fsm/include/bb_fsm.h). ---

void test_wifi_prov_unmatched_event_in_idle_is_noop(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_SAVE_SUCCESS, NULL));
    TEST_ASSERT_EQUAL_INT(WP_IDLE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.prov_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.log_calls);
}

void test_wifi_prov_unmatched_event_in_active_is_noop(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_ACTIVE);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_fsm_step(&ctx.fsm, EV_ENTRY_FIRST_BOOT, NULL));
    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.prov_start_calls);
    // Only the on_entry portal-open call from fixture_init() counts.
    TEST_ASSERT_EQUAL(1, s_fake.log_calls);
}

// --- An entry event with no matching row from the current state (a stray
// EV_ENTRY_FIRST_BOOT while already WP_ACTIVE) stops the driver at hop 1: no
// adapter calls, state unchanged. ---

void test_wifi_prov_drive_entry_unmatched_entry_event_is_noop(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_ACTIVE);

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_FIRST_BOOT);

    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, s_fake.ap_start_calls);
    TEST_ASSERT_EQUAL(0, s_fake.prov_start_calls);
    // Only the on_entry portal-open call from fixture_init() counts.
    TEST_ASSERT_EQUAL(1, s_fake.log_calls);
}

// --- NULL ctx is a no-op (documented contract on wifi_prov_fsm_drive_entry) ---

void test_wifi_prov_drive_entry_null_ctx_is_noop(void)
{
    wifi_prov_fsm_drive_entry(NULL, EV_ENTRY_FIRST_BOOT);
}

// --- Table invariant guardrail (LOW finding fix) --------------------------
//
// wifi_prov_fsm_drive_entry's hops 2/3 feed EV_AP_START_OK/EV_PROV_START_OK
// via bb_fsm_step WITHOUT re-checking its return -- see the two-hop
// rationale comment above act_try_prov_start in wifi_prov_policy.c and the
// call sites in wifi_prov_fsm_drive_entry itself. That omission is sound
// only as long as, from WP_IDLE, rows (WP_IDLE, EV_AP_START_OK) and
// (WP_IDLE, EV_PROV_START_OK) both (a) have no guard that could reject the
// match and (b) are not shadowed by an earlier BB_FSM_STATE_ANY row for the
// same event. These two tests feed each event directly (bypassing
// wifi_prov_fsm_drive_entry) from WP_IDLE and assert BB_OK plus the
// expected action ran -- this fails if a future edit adds a guard to
// either row, or inserts a shadowing ANY row ahead of it in table order.
//
// NOT covered: "exactly one row matches this (state,event) pair" -- a
// single bb_fsm_step call cannot distinguish one matching row from two
// identical duplicate rows (both produce the same observable call/return).
// Detecting that would need a table accessor (there is none -- the table
// is file-static, following wifi_reconn_policy.c's precedent of not
// exposing one for tests); flagged here rather than silently claimed.
//
// If either row above is ever changed to need this simplification revisited
// (guard added, or an FSM step return actually re-checked in the driver),
// start here.

void test_wifi_prov_ap_start_ok_row_matches_unguarded_from_idle(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_AP_START_OK, NULL));

    TEST_ASSERT_EQUAL_INT(WP_IDLE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.prov_start_calls);
    TEST_ASSERT_TRUE(ctx.last_op_ok);
}

void test_wifi_prov_prov_start_ok_row_matches_unguarded_from_idle(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_PROV_START_OK, NULL));

    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));
}

// --- settle_ms==0 clamp (MEDIUM finding fix) -------------------------------
//
// bb_fsm_arm_timer treats ms==0 as DISARM, not immediate-fire, and
// EV_SETTLE_TIMEOUT is WP_CLOSING's only exit -- so a raw 0 would strand the
// FSM there forever. wifi_prov_fsm_init clamps a 0 up to 1 before it's ever
// stashed in ctx->settle_ms. These tests bypass fixture_init (which always
// passes TEST_SETTLE_MS) to exercise the clamp directly.

void test_wifi_prov_fsm_init_clamps_zero_settle_ms_to_one(void)
{
    wifi_prov_ctx_t ctx;
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.ap_start_result = BB_OK;
    s_fake.prov_start_result = BB_OK;
    memset(&ctx, 0, sizeof(ctx));
    ctx.adapter = &s_adapter;

    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_fsm_init(&ctx, WP_IDLE, 0));

    TEST_ASSERT_EQUAL_UINT32(1u, ctx.settle_ms);
}

void test_wifi_prov_fsm_init_nonzero_settle_ms_unclamped(void)
{
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    TEST_ASSERT_EQUAL_UINT32(TEST_SETTLE_MS, ctx.settle_ms);
}

// --- zero settle_ms still arms a real (1ms), fireable timer on WP_CLOSING
// entry, and the FSM still completes the full cycle back to WP_IDLE with
// teardown running in the documented order ---

void test_wifi_prov_direct_zero_settle_ms_write_after_init_still_floored(void)
{
    // settle_ms==0 written DIRECTLY after a normal init (bypassing the
    // constructor clamp) is caught at point of use by wp_closing_on_entry's
    // own settle_ms_floor call -- the case the constructor clamp alone
    // can't cover (LOW finding fix).
    wifi_prov_ctx_t ctx;
    fixture_init(&ctx, WP_IDLE);

    ctx.settle_ms = 0; // bypass wifi_prov_fsm_init's clamp entirely

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_FIRST_BOOT);
    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_SAVE_SUCCESS, NULL));
    TEST_ASSERT_EQUAL_INT(WP_CLOSING, bb_fsm_state(&ctx.fsm));

    TEST_ASSERT_EQUAL(1, bb_fsm_timer_count(&ctx.fsm));
    bb_fsm_event_t armed_evt;
    uint32_t armed_ms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &armed_evt, &armed_ms));
    TEST_ASSERT_EQUAL(EV_SETTLE_TIMEOUT, armed_evt);
    TEST_ASSERT_EQUAL_UINT32(1u, armed_ms);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_SETTLE_TIMEOUT, NULL));

    TEST_ASSERT_EQUAL_INT(WP_IDLE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, bb_fsm_timer_count(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.prov_stop_calls);
    TEST_ASSERT_EQUAL(1, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(1, s_fake.creds_notify_calls);
    TEST_ASSERT_EQUAL(1, s_fake.prov_stop_seq);
    TEST_ASSERT_EQUAL(2, s_fake.ap_stop_seq);
    TEST_ASSERT_EQUAL(3, s_fake.creds_notify_seq);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_PORTAL_CLOSED, s_fake.log_hist[s_fake.log_hist_n - 1]);
}

void test_wifi_prov_zero_settle_ms_arms_one_ms_timer_and_completes_cycle(void)
{
    wifi_prov_ctx_t ctx;
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.ap_start_result = BB_OK;
    s_fake.prov_start_result = BB_OK;
    memset(&ctx, 0, sizeof(ctx));
    ctx.adapter = &s_adapter;
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_fsm_init(&ctx, WP_IDLE, 0));

    wifi_prov_fsm_drive_entry(&ctx, EV_ENTRY_FIRST_BOOT);
    TEST_ASSERT_EQUAL_INT(WP_ACTIVE, bb_fsm_state(&ctx.fsm));

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_SAVE_SUCCESS, NULL));
    TEST_ASSERT_EQUAL_INT(WP_CLOSING, bb_fsm_state(&ctx.fsm));

    TEST_ASSERT_EQUAL(1, bb_fsm_timer_count(&ctx.fsm));
    bb_fsm_event_t armed_evt;
    uint32_t armed_ms;
    TEST_ASSERT_TRUE(bb_fsm_timer_at(&ctx.fsm, 0, &armed_evt, &armed_ms));
    TEST_ASSERT_EQUAL(EV_SETTLE_TIMEOUT, armed_evt);
    TEST_ASSERT_EQUAL_UINT32(1u, armed_ms);

    TEST_ASSERT_EQUAL(BB_OK, bb_fsm_step(&ctx.fsm, EV_SETTLE_TIMEOUT, NULL));

    TEST_ASSERT_EQUAL_INT(WP_IDLE, bb_fsm_state(&ctx.fsm));
    TEST_ASSERT_EQUAL(0, bb_fsm_timer_count(&ctx.fsm));
    TEST_ASSERT_EQUAL(1, s_fake.prov_stop_calls);
    TEST_ASSERT_EQUAL(1, s_fake.ap_stop_calls);
    TEST_ASSERT_EQUAL(1, s_fake.creds_notify_calls);
    TEST_ASSERT_EQUAL(1, s_fake.prov_stop_seq);
    TEST_ASSERT_EQUAL(2, s_fake.ap_stop_seq);
    TEST_ASSERT_EQUAL(3, s_fake.creds_notify_seq);
    TEST_ASSERT_EQUAL(WIFI_PROV_LOG_PORTAL_CLOSED, s_fake.log_hist[s_fake.log_hist_n - 1]);
}

// ===========================================================================
// B1-809 PR4 -- wifi_prov_entry_decision: pure boot-time entry policy.
// Exhausts all 4 (has_creds, provisioned) combinations plus the NULL
// out_event behavior. See wifi_prov_policy.h for the full rationale this
// pins down.
// ===========================================================================

void test_wifi_prov_entry_decision_no_creds_not_provisioned_is_first_boot(void)
{
    wp_event_t ev = (wp_event_t)-1;
    TEST_ASSERT_TRUE(wifi_prov_entry_decision(false, false, &ev));
    TEST_ASSERT_EQUAL(EV_ENTRY_FIRST_BOOT, ev);
}

void test_wifi_prov_entry_decision_no_creds_provisioned_is_deprov_anomaly(void)
{
    wp_event_t ev = (wp_event_t)-1;
    TEST_ASSERT_TRUE(wifi_prov_entry_decision(false, true, &ev));
    TEST_ASSERT_EQUAL(EV_ENTRY_DEPROV_ANOMALY, ev);
}

void test_wifi_prov_entry_decision_has_creds_not_provisioned_no_portal(void)
{
    wp_event_t ev = (wp_event_t)-1;
    TEST_ASSERT_FALSE(wifi_prov_entry_decision(true, false, &ev));
    TEST_ASSERT_EQUAL((wp_event_t)-1, ev); // untouched
}

void test_wifi_prov_entry_decision_has_creds_provisioned_no_portal(void)
{
    wp_event_t ev = (wp_event_t)-1;
    TEST_ASSERT_FALSE(wifi_prov_entry_decision(true, true, &ev));
    TEST_ASSERT_EQUAL((wp_event_t)-1, ev); // untouched
}

void test_wifi_prov_entry_decision_null_out_event_is_tolerated(void)
{
    TEST_ASSERT_TRUE(wifi_prov_entry_decision(false, false, NULL));
    TEST_ASSERT_TRUE(wifi_prov_entry_decision(false, true, NULL));
    TEST_ASSERT_FALSE(wifi_prov_entry_decision(true, false, NULL));
    TEST_ASSERT_FALSE(wifi_prov_entry_decision(true, true, NULL));
}
