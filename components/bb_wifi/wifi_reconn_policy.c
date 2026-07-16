#include "wifi_reconn_policy.h"

void wifi_reconn_state_reset(wifi_reconn_state_t *st)
{
    if (!st) return;
    st->handshake_fail_count = 0;
    st->generic_fail_count = 0;
    st->first_fail_us = 0;
    st->retry_count = 0;
    st->last_reason = 0;
    st->last_disconnect_us = 0;
    st->lost_ip_count = 0;
    st->last_lost_ip_us = 0;
    st->egress_fail_streak = 0;
    st->egress_dead_count = 0;
    st->last_egress_dead_us = 0;
    for (int i = 0; i < BB_WIFI_DISC_COUNT; i++) {
        st->reason_histogram[i] = 0;
    }
}

static uint32_t compute_backoff_ms(const wifi_reconn_state_t *st, bb_wifi_disc_reason_t reason)
{
    if (reason == BB_WIFI_DISC_HANDSHAKE_TIMEOUT) {
        int n = st->handshake_fail_count;
        if (n <= WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT)
            return 0;
        if (n <= WIFI_RECONN_HANDSHAKE_TIER2_LIMIT)
            return WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS;
        return WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS;
    }
    int n = st->generic_fail_count;
    if (n <= WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT)
        return 0;
    return WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS;
}

wifi_reconn_action_t wifi_reconn_policy_on_disconnect(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a,
    bb_wifi_disc_reason_t reason, uint32_t *backoff_ms_out)
{
    if (!st || !a || !backoff_ms_out) {
        return WIFI_RECONN_ACTION_NONE;
    }

    int64_t now = a->now_us();

    // Update diagnostic state.
    st->last_reason = reason;
    st->last_disconnect_us = now;
    if (st->reason_histogram[reason] < UINT16_MAX) {
        st->reason_histogram[reason]++;
    }
    st->retry_count++;

    // Track first failure time in current window.
    if (st->first_fail_us == 0) {
        st->first_fail_us = now;
    }

    // Increment appropriate counter based on reason.
    if (reason == BB_WIFI_DISC_HANDSHAKE_TIMEOUT) {
        st->handshake_fail_count++;
    } else {
        st->generic_fail_count++;
    }

    // Check if we've exceeded the persistent fail window. first_fail_us is
    // guaranteed non-zero here (set above when 0).
    if (now - st->first_fail_us > WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US) {
        return WIFI_RECONN_ACTION_REBOOT;
    }

    // Compute backoff strategy.
    uint32_t backoff_ms = compute_backoff_ms(st, reason);
    *backoff_ms_out = backoff_ms;

    if (backoff_ms == 0) {
        return WIFI_RECONN_ACTION_RECONNECT_NOW;
    }

    return WIFI_RECONN_ACTION_SCHEDULE_BACKOFF;
}

void wifi_reconn_policy_on_got_ip(wifi_reconn_state_t *st)
{
    if (!st) return;
    st->handshake_fail_count = 0;
    st->generic_fail_count = 0;
    st->first_fail_us = 0;
    st->retry_count = 0;
}

bool wifi_reconn_should_reconnect_no_ip(bool associated, bool has_ip)
{
    return associated && !has_ip;
}

void wifi_reconn_policy_on_lost_ip(wifi_reconn_state_t *st, const wifi_reconn_adapter_t *ad)
{
    if (!st || !ad) return;
    st->lost_ip_count++;
    st->last_lost_ip_us = ad->now_us();
    if (st->reason_histogram[WIFI_REASON_BB_LOST_IP] < UINT16_MAX) {
        st->reason_histogram[WIFI_REASON_BB_LOST_IP]++;
    }
}

wifi_reconn_action_t wifi_reconn_policy_on_connect_timeout(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a,
    uint32_t *backoff_ms_out)
{
    if (!st || !a || !backoff_ms_out) {
        return WIFI_RECONN_ACTION_NONE;
    }

    int64_t now = a->now_us();

    st->last_disconnect_us = now;
    st->retry_count++;
    st->generic_fail_count++;

    if (st->first_fail_us == 0) {
        st->first_fail_us = now;
    }

    if (now - st->first_fail_us > WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US) {
        return WIFI_RECONN_ACTION_REBOOT;
    }

    // Apply the same progressive backoff as on_disconnect's generic path.
    // BB_WIFI_DISC_UNKNOWN != BB_WIFI_DISC_HANDSHAKE_TIMEOUT, forcing the
    // generic branch (a connect-timeout stall is never a handshake reason).
    uint32_t backoff_ms = compute_backoff_ms(st, BB_WIFI_DISC_UNKNOWN);
    *backoff_ms_out = backoff_ms;

    if (backoff_ms == 0) {
        return WIFI_RECONN_ACTION_RECONNECT_NOW;
    }

    return WIFI_RECONN_ACTION_SCHEDULE_BACKOFF;
}

wifi_reconn_action_t wifi_reconn_policy_on_egress_probe(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *ad,
    bool reachable, int fail_threshold)
{
    if (!st || !ad) return WIFI_RECONN_ACTION_NONE;

    if (reachable) {
        st->egress_fail_streak = 0;
        return WIFI_RECONN_ACTION_NONE;
    }

    st->egress_fail_streak++;
    if (st->egress_fail_streak >= (uint8_t)fail_threshold) {
        st->egress_dead_count++;
        st->last_egress_dead_us = ad->now_us();
        if (st->reason_histogram[WIFI_REASON_BB_EGRESS_DEAD] < UINT16_MAX) {
            st->reason_histogram[WIFI_REASON_BB_EGRESS_DEAD]++;
        }
        if (st->first_fail_us == 0) {
            st->first_fail_us = ad->now_us();
        }
        st->egress_fail_streak = 0;
        return WIFI_RECONN_ACTION_RECONNECT_NOW;
    }
    return WIFI_RECONN_ACTION_NONE;
}

// ===========================================================================
// bb_fsm rebuild (B1-805 slice 1a). See wifi_reconn_policy.h for the public
// enums/ctx/desc-init entry point. Everything below is file-static: guards,
// actions, on_entry hooks, and the row/state tables.
// ===========================================================================

static uint8_t wr_evt_reason(const void *evt_data)
{
    return evt_data ? *(const uint8_t *)evt_data : 0;
}

// Pure peek: would this disconnect/timeout, if processed right now, exceed
// the persistent-fail window? Reason-independent (the window check in
// wifi_reconn_policy_on_disconnect/_on_connect_timeout doesn't key on
// reason), so this one helper backs both the "_disconnect_" and "_timeout_"
// named peek predicates the architect brief calls for (R2) -- kept as
// distinct thin wrappers below for call-site clarity, not duplicated logic.
static bool wr_would_escalate(const wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a)
{
    int64_t now = a->now_us();
    int64_t first_fail = st->first_fail_us ? st->first_fail_us : now;
    return (now - first_fail) > WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US;
}

static bool wr_disconnect_would_escalate(const wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a)
{
    return wr_would_escalate(st, a);
}

static bool wr_timeout_would_escalate(const wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a)
{
    return wr_would_escalate(st, a);
}

// Pure peek re-derivation of compute_backoff_ms's outcome AS IF this
// disconnect were processed -- the +1 POST-increment count (matching what
// the action's mutating wifi_reconn_policy_on_disconnect call will do), not
// the raw pre-increment count (R2 -- avoids an off-by-one at tier
// boundaries).
static bool wr_disconnect_would_reconnect_now(const wifi_reconn_state_t *st, bb_wifi_disc_reason_t reason)
{
    wifi_reconn_state_t peek = *st;
    if (reason == BB_WIFI_DISC_HANDSHAKE_TIMEOUT) {
        peek.handshake_fail_count++;
    } else {
        peek.generic_fail_count++;
    }
    return compute_backoff_ms(&peek, reason) == 0;
}

// --- Guards (pure: no mutation, no emit -- bb_fsm_guard_fn contract) ---

static bool guard_disc_escalate_allowed(void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    if (!wr_disconnect_would_escalate(&ctx->policy, ctx->adapter)) return false;
    return ctx->adapter->reboot_allowed_fn();
}

static bool guard_disc_escalate_denied(void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    // Matches only when row 4 (guard_disc_escalate_allowed) already failed --
    // i.e. would_escalate is true but reboot_allowed_fn denied it.
    return wr_disconnect_would_escalate(&ctx->policy, ctx->adapter);
}

static bool guard_disc_reconnect_now(void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event;
    wifi_reconn_ctx_t *ctx = vctx;
    bb_wifi_disc_reason_t reason = bb_wifi_map_esp_reason(wr_evt_reason(evt_data));
    return wr_disconnect_would_reconnect_now(&ctx->policy, reason);
}

static bool guard_timeout_escalate_allowed(void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    if (!wr_timeout_would_escalate(&ctx->policy, ctx->adapter)) return false;
    return ctx->adapter->reboot_allowed_fn();
}

static bool guard_timeout_escalate_denied(void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    return wr_timeout_would_escalate(&ctx->policy, ctx->adapter);
}

// --- Actions (own all mutation/side-effects; every esp_wifi_*/bb_system_*/
// reboot/emit call goes through ctx->adapter -- zero bare inline calls) ---

static void act_reset_state(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    wifi_reconn_state_reset(&ctx->policy);
    ctx->self_disconnect = false;
    ctx->adapter->connect_fn();
}

static void act_on_got_ip(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    wifi_reconn_policy_on_got_ip(&ctx->policy);
    ctx->pending_backoff_ms = 0;
}

// Bookkeeping only for the ESCALATE (allowed) edge -- the guard already
// decided; this call's return value is ignored, only the mutation
// (histogram/last_reason/counters) matters.
static void act_note_disconnect(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event;
    wifi_reconn_ctx_t *ctx = vctx;
    bb_wifi_disc_reason_t reason = bb_wifi_map_esp_reason(wr_evt_reason(evt_data));
    uint32_t backoff_ms = 0;
    (void)wifi_reconn_policy_on_disconnect(&ctx->policy, ctx->adapter, reason, &backoff_ms);
}

// R14 guard-placement deny path: record the disconnect, emit
// BB_WIFI_NET_EVT_REBOOT_DENIED, and stash a fallback backoff. The guard
// already established would_escalate (the persistent-fail window is
// exceeded), so the mutating call's own identical window check always
// short-circuits into its REBOOT branch too (returning before it ever
// computes a backoff) -- backoff_ms_out is deliberately NOT read here (it
// would only ever observe its initial 0); the generic pause tier is used
// unconditionally so the denied path never tight-loops.
static void act_disconnect_denied(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event;
    wifi_reconn_ctx_t *ctx = vctx;
    bb_wifi_disc_reason_t reason = bb_wifi_map_esp_reason(wr_evt_reason(evt_data));
    uint32_t backoff_ms = 0;
    (void)wifi_reconn_policy_on_disconnect(&ctx->policy, ctx->adapter, reason, &backoff_ms);
    ctx->pending_backoff_ms = WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS;
    ctx->adapter->emit_net_event_fn(BB_WIFI_NET_EVT_REBOOT_DENIED, reason);
}

// Rows 5/9 (SAME on WR_CONNECTING, concrete on WR_CONNECTED->WR_CONNECTING).
// SAME does NOT re-run on_entry, so this explicitly re-arms the CONNECTING
// watchdog; on the concrete WR_CONNECTED->WR_CONNECTING row the arm is
// harmlessly cleared by the post-action clear and re-armed for real by
// WR_CONNECTING's own on_entry.
static void act_disconnect_reconnect_now(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event;
    wifi_reconn_ctx_t *ctx = vctx;
    bb_wifi_disc_reason_t reason = bb_wifi_map_esp_reason(wr_evt_reason(evt_data));
    uint32_t backoff_ms = 0;
    (void)wifi_reconn_policy_on_disconnect(&ctx->policy, ctx->adapter, reason, &backoff_ms);
    ctx->adapter->connect_fn();
    bb_fsm_arm_timer(fsm, EV_CONNECTING_TIMEOUT, WIFI_RECONN_CONNECTING_TIMEOUT_MS);
}

// Rows 6/10 (default disconnect path -> WR_BACKOFF). Stashes the computed
// backoff for WR_BACKOFF's on_entry to arm.
static void act_disconnect_backoff(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event;
    wifi_reconn_ctx_t *ctx = vctx;
    bb_wifi_disc_reason_t reason = bb_wifi_map_esp_reason(wr_evt_reason(evt_data));
    uint32_t backoff_ms = 0;
    (void)wifi_reconn_policy_on_disconnect(&ctx->policy, ctx->adapter, reason, &backoff_ms);
    ctx->pending_backoff_ms = backoff_ms;
}

// Bookkeeping only for the timeout-ESCALATE (allowed) edge.
static void act_note_timeout(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    uint32_t backoff_ms = 0;
    (void)wifi_reconn_policy_on_connect_timeout(&ctx->policy, ctx->adapter, &backoff_ms);
}

// R14 guard-placement deny path for the connect-timeout escalate edge.
// Same reasoning as act_disconnect_denied: the guard already established
// would_escalate, so the mutating call's identical window check always
// short-circuits into REBOOT before computing a backoff -- the generic
// pause tier is used unconditionally. BB_WIFI_DISC_UNKNOWN is the "not
// meaningful" reason (a stall has no esp disconnect reason code), matching
// the GOT_IP convention documented on bb_wifi_net_event_t.
static void act_timeout_denied(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    uint32_t backoff_ms = 0;
    (void)wifi_reconn_policy_on_connect_timeout(&ctx->policy, ctx->adapter, &backoff_ms);
    ctx->pending_backoff_ms = WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS;
    ctx->adapter->emit_net_event_fn(BB_WIFI_NET_EVT_REBOOT_DENIED, BB_WIFI_DISC_UNKNOWN);
}

// Row 8: connect-timeout default -- re-attempt WITHOUT teardown, NEVER
// leaving WR_CONNECTING (SAME). Sets self_disconnect so the shell's
// disconnect notifier absorbs the resulting WIFI_EVENT_STA_DISCONNECTED
// instead of feeding it back into this FSM as a second event.
static void act_timeout_reattempt(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    uint32_t backoff_ms = 0;
    (void)wifi_reconn_policy_on_connect_timeout(&ctx->policy, ctx->adapter, &backoff_ms);
    ctx->self_disconnect = true;
    ctx->adapter->disconnect_fn();
    ctx->adapter->connect_fn();
    bb_fsm_arm_timer(fsm, EV_CONNECTING_TIMEOUT, WIFI_RECONN_CONNECTING_TIMEOUT_MS);
}

static void act_backoff_elapsed(bb_fsm_t *fsm, void *vctx, bb_fsm_event_t event, void *evt_data)
{
    (void)fsm; (void)event; (void)evt_data;
    wifi_reconn_ctx_t *ctx = vctx;
    ctx->adapter->connect_fn();
}

// --- State on_entry hooks ---

static void wr_connecting_on_entry(bb_fsm_t *fsm, void *vctx, bb_fsm_state_t state)
{
    (void)vctx; (void)state;
    bb_fsm_arm_timer(fsm, EV_CONNECTING_TIMEOUT, WIFI_RECONN_CONNECTING_TIMEOUT_MS);
}

static void wr_backoff_on_entry(bb_fsm_t *fsm, void *vctx, bb_fsm_state_t state)
{
    (void)state;
    wifi_reconn_ctx_t *ctx = vctx;
    bb_fsm_arm_timer(fsm, EV_BACKOFF_TIMEOUT, ctx->pending_backoff_ms);
}

// R14 unconditional reboot -- reached ONLY on an allowed escalation (the
// guard already gated it); no predicate re-check, no deny branch here.
// Accounting (boot-fail bump + budget record) now lives inside reboot_fn
// itself (bb_system_safeguard_reboot), not spread across separate hooks.
static void wr_escalate_on_entry(bb_fsm_t *fsm, void *vctx, bb_fsm_state_t state)
{
    (void)fsm; (void)state;
    wifi_reconn_ctx_t *ctx = vctx;
    ctx->adapter->reboot_fn("persistent disconnect");
}

// --- Table (row order is the match order -- see bb_fsm_step's contract) ---

static const bb_fsm_row_t wr_rows[] = {
    // 1. WR_NO_CREDS is terminal-until-provisioned: this is the ONLY row
    //    with state==WR_NO_CREDS, and no BB_FSM_STATE_ANY row below matches
    //    it (see the no-creds safety invariant on wifi_reconn_fsm_init).
    { WR_NO_CREDS,   EV_CREDS_ARRIVED,      NULL,                            act_reset_state,               WR_CONNECTING },

    // 2-8: WR_CONNECTING
    { WR_CONNECTING, EV_GOT_IP,             NULL,                            act_on_got_ip,                  WR_CONNECTED },
    { WR_CONNECTING, EV_STA_CONNECTED,      NULL,                            NULL,                            BB_FSM_STATE_SAME },
    { WR_CONNECTING, EV_STA_DISCONNECTED,   guard_disc_escalate_allowed,     act_note_disconnect,            WR_ESCALATE_REBOOT },
    { WR_CONNECTING, EV_STA_DISCONNECTED,   guard_disc_escalate_denied,      act_disconnect_denied,          WR_BACKOFF },
    { WR_CONNECTING, EV_STA_DISCONNECTED,   guard_disc_reconnect_now,        act_disconnect_reconnect_now,   BB_FSM_STATE_SAME },
    { WR_CONNECTING, EV_STA_DISCONNECTED,   NULL,                            act_disconnect_backoff,         WR_BACKOFF },
    { WR_CONNECTING, EV_CONNECTING_TIMEOUT, guard_timeout_escalate_allowed,  act_note_timeout,                WR_ESCALATE_REBOOT },
    { WR_CONNECTING, EV_CONNECTING_TIMEOUT, guard_timeout_escalate_denied,   act_timeout_denied,             WR_BACKOFF },
    { WR_CONNECTING, EV_CONNECTING_TIMEOUT, NULL,                            act_timeout_reattempt,          BB_FSM_STATE_SAME },

    // 9-10: WR_CONNECTED
    { WR_CONNECTED,  EV_STA_DISCONNECTED,   guard_disc_reconnect_now,        act_disconnect_reconnect_now,   WR_CONNECTING },
    { WR_CONNECTED,  EV_STA_DISCONNECTED,   NULL,                            act_disconnect_backoff,         WR_BACKOFF },

    // 11-12: WR_BACKOFF
    { WR_BACKOFF,    EV_BACKOFF_TIMEOUT,    NULL,                            act_backoff_elapsed,            WR_CONNECTING },
    { WR_BACKOFF,    EV_STA_DISCONNECTED,   NULL,                            NULL,                            BB_FSM_STATE_SAME },
};

static const bb_fsm_state_desc_t wr_states[] = {
    { WR_CONNECTING,      wr_connecting_on_entry, NULL },
    { WR_BACKOFF,         wr_backoff_on_entry,    NULL },
    { WR_ESCALATE_REBOOT, wr_escalate_on_entry,   NULL },
};

bb_err_t wifi_reconn_fsm_init(wifi_reconn_ctx_t *ctx, bb_fsm_state_t initial)
{
    // ctx->desc is embedded IN ctx (not a caller-local stack variable) --
    // bb_fsm_init() stores the pointer it's given verbatim (fsm->desc =
    // desc), it does not copy the pointed-to struct, so the desc must
    // outlive the fsm. See the field comment on wifi_reconn_ctx_t.
    ctx->desc.rows = wr_rows;
    ctx->desc.row_count = sizeof(wr_rows) / sizeof(wr_rows[0]);
    ctx->desc.states = wr_states;
    ctx->desc.state_count = sizeof(wr_states) / sizeof(wr_states[0]);
    ctx->desc.initial = initial;
    ctx->desc.ctx = ctx;
    return bb_fsm_init(&ctx->fsm, &ctx->desc);
}
