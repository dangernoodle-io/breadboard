#include "wifi_prov_policy.h"

// ===========================================================================
// WHY THE TWO-HOP ENTRY (do not "simplify" it):
//
// bb_fsm guards must be PURE (bb_fsm_guard_fn takes no fsm handle -- it
// cannot arm timers or mutate FSM state, and by contract must not have
// side effects). But ap_start_fn()/prov_start_fn() are SYNCHRONOUS calls
// whose success decides the NEXT state -- a guard cannot try a side effect
// just to decide which row matches.
//
// So the table splits each attempt into its own action (act_try_ap_start,
// act_try_prov_start), each of which makes exactly one adapter call and
// stashes the bb_err_t outcome in ctx->last_op_ok. Both actions' rows
// target BB_FSM_STATE_SAME regardless of outcome -- the FSM step alone
// never decides success/failure.
//
// The DRIVER (wifi_prov_fsm_drive_entry below) -- NEVER a guard, NEVER an
// action -- is the only thing that reads ctx->last_op_ok between hops and
// decides whether to feed the next synthetic event (EV_AP_START_OK /
// EV_PROV_START_OK). On a failed hop it simply stops calling bb_fsm_step
// again; the FSM is already sitting in WP_IDLE (SAME kept it there), which
// IS "log and stay parked" -- no separate failure state is needed.
//
// A future reader may be tempted to "simplify" this into a single action
// that starts AP and provisioning together, or into a guard that "checks"
// ap_start_fn's return -- both would violate the guard-purity contract
// (bb_fsm_guard_fn contract, bb_fsm.h) or collapse two independently
// failable side effects into one non-recoverable step. Don't.
// ===========================================================================

static wifi_prov_log_evt_t entry_log_evt(wp_event_t event)
{
    if (event == EV_ENTRY_FIRST_BOOT) return WIFI_PROV_LOG_ENTRY_FIRST_BOOT;
    if (event == EV_ENTRY_DEPROV_ANOMALY) return WIFI_PROV_LOG_ENTRY_DEPROV_ANOMALY;
    return WIFI_PROV_LOG_ENTRY_USER_REQUESTED;
}

// --- Actions (own all mutation/side-effects; every adapter call goes
// through ctx->adapter -- zero bare inline calls) ---

// Rows 1-3 (WP_IDLE + any entry event): attempt ap_start_fn(). Success or
// failure is stashed for the driver; on failure this is the terminal
// outcome for this entry attempt -- no rollback needed (nothing was
// started yet).
static void act_try_ap_start(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)evt_data;
    wifi_prov_ctx_t *ctx = vctx;
    ctx->adapter->log_fn(entry_log_evt((wp_event_t)event));
    ctx->last_op_ok = (ctx->adapter->ap_start_fn() == BB_OK);
    if (!ctx->last_op_ok) {
        ctx->adapter->log_fn(WIFI_PROV_LOG_AP_START_FAILED);
    }
}

// Row 4 (WP_IDLE + EV_AP_START_OK, posted by the driver after a successful
// ap_start): attempt prov_start_fn(). On failure, roll back the AP that was
// already brought up -- leaving a SoftAP up with no portal behind it is the
// failure mode this rollback prevents.
static void act_try_prov_start(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_prov_ctx_t *ctx = vctx;
    ctx->last_op_ok = (ctx->adapter->prov_start_fn() == BB_OK);
    if (!ctx->last_op_ok) {
        ctx->adapter->ap_stop_fn();
        ctx->adapter->log_fn(WIFI_PROV_LOG_PROV_START_FAILED);
    }
}

// Row 6 (WP_ACTIVE + EV_ENTRY_USER_REQUESTED): re-entry while already
// active -- no AP/portal restart, just a diagnostic.
static void act_reentry_noop(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_prov_ctx_t *ctx = vctx;
    ctx->adapter->log_fn(WIFI_PROV_LOG_ENTRY_ALREADY_ACTIVE);
}

// Row 7 (WP_ACTIVE + EV_SAVE_SUCCESS -> WP_IDLE): close the portal. Exactly
// these three calls, in this order -- prov_stop_fn, then ap_stop_fn, then
// creds_arrived_notify_fn (the PR1 seam that lets the reconnect FSM pick up
// the freshly-saved credentials).
static void act_close_portal(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_prov_ctx_t *ctx = vctx;
    ctx->adapter->prov_stop_fn();
    ctx->adapter->ap_stop_fn();
    ctx->adapter->creds_arrived_notify_fn();
}

// --- State on_entry hooks ---

static void wp_active_on_entry(bb_fsm_t *fsm, void *vctx, bb_fsm_state_t state)
{
    (void)fsm; (void)state;
    wifi_prov_ctx_t *ctx = vctx;
    ctx->adapter->log_fn(WIFI_PROV_LOG_PORTAL_OPEN);
}

// --- Table (row order is the match order -- see bb_fsm_step's contract) ---

static const bb_fsm_row_t wp_rows[] = {
    { WP_IDLE,   EV_ENTRY_FIRST_BOOT,     NULL, act_try_ap_start,   BB_FSM_STATE_SAME },
    { WP_IDLE,   EV_ENTRY_DEPROV_ANOMALY, NULL, act_try_ap_start,   BB_FSM_STATE_SAME },
    { WP_IDLE,   EV_ENTRY_USER_REQUESTED, NULL, act_try_ap_start,   BB_FSM_STATE_SAME },
    { WP_IDLE,   EV_AP_START_OK,          NULL, act_try_prov_start, BB_FSM_STATE_SAME },
    { WP_IDLE,   EV_PROV_START_OK,        NULL, NULL,               WP_ACTIVE },
    { WP_ACTIVE, EV_ENTRY_USER_REQUESTED, NULL, act_reentry_noop,   BB_FSM_STATE_SAME },
    { WP_ACTIVE, EV_SAVE_SUCCESS,         NULL, act_close_portal,   WP_IDLE },
};

static const bb_fsm_state_desc_t wp_states[] = {
    { WP_ACTIVE, wp_active_on_entry, NULL },
};

bb_err_t wifi_prov_fsm_init(wifi_prov_ctx_t *ctx, bb_fsm_state_t initial)
{
    // ctx->desc is embedded IN ctx (not a caller-local stack variable) --
    // bb_fsm_init() stores the pointer it's given verbatim, it does not
    // copy the pointed-to struct, so the desc must outlive the fsm. See the
    // field comment on wifi_prov_ctx_t.
    ctx->desc.rows = wp_rows;
    ctx->desc.row_count = sizeof(wp_rows) / sizeof(wp_rows[0]);
    ctx->desc.states = wp_states;
    ctx->desc.state_count = sizeof(wp_states) / sizeof(wp_states[0]);
    ctx->desc.initial = initial;
    ctx->desc.ctx = ctx;
    return bb_fsm_init(&ctx->fsm, &ctx->desc);
}

void wifi_prov_fsm_drive_entry(wifi_prov_ctx_t *ctx, wp_event_t entry_event)
{
    if (!ctx) return;
    ctx->last_op_ok = false;

    // Hop 1: the entry event itself. A caller driving an event/state
    // combination absent from the table (e.g. a stray EV_ENTRY_FIRST_BOOT
    // while already WP_ACTIVE) is a safe no-op -- bb_fsm_step returns
    // BB_ERR_NOT_FOUND, state unchanged, and this stops the chain here.
    if (bb_fsm_step(&ctx->fsm, entry_event, NULL) != BB_OK) return;

    // From WP_ACTIVE this ran act_reentry_noop (row 6, SAME) -- no further
    // hops. Only WP_IDLE (rows 1-3 ran act_try_ap_start) continues.
    if (bb_fsm_state(&ctx->fsm) != WP_IDLE) return;
    if (!ctx->last_op_ok) return; // ap_start failed -- stays WP_IDLE, parked

    // Hop 2: ap_start succeeded -- attempt prov_start (row 4). This event
    // always matches from WP_IDLE (row 4 is unconditional, no guard), so
    // its bb_fsm_step return is not re-checked here; only the outcome
    // act_try_prov_start stashed matters. Failure here rolls back inside
    // the action itself (ap_stop_fn) and leaves us in WP_IDLE.
    // Guardrail: this omission is pinned by
    // test_wifi_prov_ap_start_ok_row_matches_unguarded_from_idle in
    // test_wifi_prov_policy.c -- if you add a guard to row 4 or shadow it
    // with an earlier BB_FSM_STATE_ANY row, that test fails.
    bb_fsm_step(&ctx->fsm, EV_AP_START_OK, NULL);
    if (!ctx->last_op_ok) return; // prov_start failed -- rolled back, parked

    // Hop 3: prov_start succeeded -- commit to WP_ACTIVE (row 5, also
    // unconditional from WP_IDLE).
    // Guardrail: pinned by
    // test_wifi_prov_prov_start_ok_row_matches_unguarded_from_idle in
    // test_wifi_prov_policy.c (same rationale as hop 2's).
    bb_fsm_step(&ctx->fsm, EV_PROV_START_OK, NULL);
}
