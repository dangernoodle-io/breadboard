#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bb_fsm.h"

// ---------------------------------------------------------------------------
// bb_fsm provisioning-orchestrator policy (B1-809 PR2) -- table-driven FSM
// deciding when SoftAP + the provisioning HTTP routes come up and go down.
// Host-compilable (no ESP-IDF/HTTP types leak in); the ESP-IDF shell (PR3)
// embeds a bb_fsm_t built from wifi_prov_fsm_init() and drives it from the
// provisioning-entry call site (boot-time first-boot/deprov-anomaly checks,
// or a user-requested re-provision route). Guards/actions/hooks are
// file-static in wifi_prov_policy.c; only the enums, ctx struct, adapter,
// and the init/drive entry points are public.
// ---------------------------------------------------------------------------

// FSM states.
//   WP_IDLE    resting/parked state -- initial, and also where every entry
//              path lands back on any failure (never leaves a SoftAP up
//              with no portal behind it, never leaves the portal
//              half-started).
//   WP_ACTIVE  AP + provisioning routes up, awaiting POST /save.
//   WP_CLOSING save succeeded; portal teardown is deferred behind a settle
//              timer so the in-flight httpd response can flush before
//              ap_stop_fn tears the netif out from under it. See the
//              "WHY THE SETTLE STATE" block below.
typedef enum {
    WP_IDLE = 0,
    WP_ACTIVE = 1,
    WP_CLOSING = 2,
} wp_state_t;

// FSM events.
//   EV_ENTRY_FIRST_BOOT      no creds on boot -- enter provisioning.
//   EV_ENTRY_DEPROV_ANOMALY  creds present but a deprovisioning anomaly was
//                            detected -- enter provisioning defensively.
//   EV_ENTRY_USER_REQUESTED  user explicitly asked to (re-)provision.
//   EV_AP_START_OK           synthetic, posted ONLY by this module's own
//                            driver (wifi_prov_fsm_drive_entry) after
//                            act_try_ap_start's ap_start_fn call succeeded --
//                            never posted by a consumer directly.
//   EV_PROV_START_OK         synthetic, posted ONLY by the driver after
//                            act_try_prov_start's prov_start_fn call
//                            succeeded -- never posted by a consumer
//                            directly.
//   EV_SAVE_SUCCESS          POST /save completed -- start closing the
//                            portal (enter the settle state; teardown is
//                            deferred, not immediate -- see wifi_prov_policy.c).
//   EV_SETTLE_TIMEOUT        settle timer (armed on WP_CLOSING entry)
//                            expired -- actually tear the portal down now.
typedef enum {
    EV_ENTRY_FIRST_BOOT = 0,
    EV_ENTRY_DEPROV_ANOMALY = 1,
    EV_ENTRY_USER_REQUESTED = 2,
    EV_AP_START_OK = 3,
    EV_PROV_START_OK = 4,
    EV_SAVE_SUCCESS = 5,
    EV_SETTLE_TIMEOUT = 6,
} wp_event_t;

// Structured diagnostic log events -- the single slot every guard/action/
// hook logs through (adapter.log_fn), so no bare ESP_LOGx/printf call is
// buried in this host-compiled TU.
typedef enum {
    WIFI_PROV_LOG_ENTRY_FIRST_BOOT = 0,
    WIFI_PROV_LOG_ENTRY_DEPROV_ANOMALY,
    WIFI_PROV_LOG_ENTRY_USER_REQUESTED,
    WIFI_PROV_LOG_ENTRY_ALREADY_ACTIVE,
    WIFI_PROV_LOG_AP_START_FAILED,
    WIFI_PROV_LOG_PROV_START_FAILED,
    WIFI_PROV_LOG_PORTAL_OPEN,
    WIFI_PROV_LOG_PORTAL_CLOSED,
} wifi_prov_log_evt_t;

// Adapter: every side-effecting call an FSM action or hook makes goes
// THROUGH one of these pointers -- zero bare inline calls in this pure
// policy TU. Real wiring: a single file-static instance in the ESP-IDF
// shell (PR3), closing over the bb_http_asset_t array / extra-routes
// callback that this header must not see (bb_wifi_prov_start's signature
// takes those; prov_start_fn here is deliberately zero-arg). All pointers
// are a required part of the contract (not optionally NULL) -- callers
// populate every field.
typedef struct {
    bb_err_t (*ap_start_fn)(void);             // bb_wifi_ap_start()
    void     (*ap_stop_fn)(void);               // bb_wifi_ap_stop()
    bb_err_t (*prov_start_fn)(void);            // bb_wifi_prov_start(assets, n, extra), closed over by the shell
    void     (*prov_stop_fn)(void);             // bb_wifi_prov_stop()
    void     (*creds_arrived_notify_fn)(void);  // bb_wifi_on_creds_arrived() (PR1 seam)
    void     (*log_fn)(wifi_prov_log_evt_t);    // structured diagnostic emit
} wifi_prov_adapter_t;

// Embedded-by-value FSM context, zero heap, single-writer (whichever task
// drives provisioning is the sole bb_fsm_step()/drive_entry caller --
// non-reentrant, never call bb_fsm_step() from within a guard/action/hook
// on this instance).
//   fsm          embedded bb_fsm_t.
//   desc         the bb_fsm_desc_t bb_fsm_init()/fsm.desc POINTS AT -- bb_fsm
//                does not copy the desc it's given, so it MUST outlive the
//                fsm; embedding it here (same lifetime as fsm/ctx) is
//                load-bearing, not style -- see wifi_reconn_ctx_t in
//                components/bb_wifi/wifi_reconn_policy.h for the same
//                dangling-pointer hazard this avoids.
//   adapter      the fn-pointer table above; const, file-static.
//   last_op_ok   set by act_try_ap_start/act_try_prov_start to the outcome
//                of the synchronous call they just made; read ONLY by
//                wifi_prov_fsm_drive_entry between hops (see the two-hop
//                entry rationale in wifi_prov_policy.c) -- never read or
//                written by a guard (guards must stay pure) or by any
//                other action.
//   settle_ms    WP_CLOSING settle-timer duration, read by wp_closing_on_entry
//                when it arms EV_SETTLE_TIMEOUT. Ctx state, not a Kconfig
//                read -- this TU is host-compilable and must stay free of
//                sdkconfig.h; the ESP-IDF shell (PR3b) populates it from
//                CONFIG_BB_WIFI_PROV_SETTLE_MS before calling
//                wifi_prov_fsm_init(), host tests pass their own value.
//                Floored at 1 both at construction (wifi_prov_fsm_init) and
//                at point of use (wp_closing_on_entry) -- see settle_ms_floor
//                in wifi_prov_policy.c for why 0 must never reach
//                bb_fsm_arm_timer. Both sites enforce it, so a direct
//                post-init write of ctx->settle_ms = 0 is safe, not merely
//                discouraged.
typedef struct {
    bb_fsm_t                    fsm;
    bb_fsm_desc_t                desc;
    const wifi_prov_adapter_t  *adapter;
    bool                         last_op_ok;
    uint32_t                     settle_ms;
} wifi_prov_ctx_t;

// Populate ctx->desc for the provisioning FSM (table + state hooks are
// file-static const in wifi_prov_policy.c) and call bb_fsm_init(&ctx->fsm,
// &ctx->desc). `initial` is always WP_IDLE in production (the shell only
// ever constructs a fresh ctx before its first entry event); host tests may
// pass WP_ACTIVE/WP_CLOSING to exercise the re-entry/close-portal rows
// directly. `settle_ms` is stashed in ctx->settle_ms BEFORE bb_fsm_init runs
// the initial state's on_entry, so passing WP_CLOSING as `initial` arms the
// settle timer with this value immediately. ctx->adapter MUST already be
// set (every guard/action/hook dereferences it). Returns bb_fsm_init's
// result.
bb_err_t wifi_prov_fsm_init(wifi_prov_ctx_t *ctx, bb_fsm_state_t initial, uint32_t settle_ms);

// Drive one provisioning-entry attempt: up to 3 bb_fsm_step calls (entry ->
// ap-start-ok -> prov-start-ok), stopping early on any failure. This is the
// ONE shared pure sequencer both the ESP-IDF shell (PR3) and the host tests
// call, so production and tests exercise IDENTICAL hop logic -- see the
// two-hop entry rationale in wifi_prov_policy.c for why this can't collapse
// into guards alone. Safe to call from WP_ACTIVE (re-entry -- a single
// no-op hop, no further steps). NULL ctx is a no-op.
void wifi_prov_fsm_drive_entry(wifi_prov_ctx_t *ctx, wp_event_t entry_event);

// ---------------------------------------------------------------------------
// Pure boot-time provisioning ENTRY decision (B1-809 PR4). This is the ONE
// place bb_settings.h's bb_settings_wifi_provisioned_get() rationale is
// encoded as code -- quoting it here so a future reader of THIS function
// sees it without having to go find the other header:
//
//   "Any future boot-time gate deciding WHETHER TO ATTEMPT AN STA CONNECT AT
//   ALL must key off wifi_has_creds(), never off this flag: a 'portal iff
//   !provisioned' gate would strand a board that has valid committed creds
//   but has not yet completed a validated connect (e.g. first boot after a
//   captive-portal save, before IP_EVENT_STA_GOT_IP has fired)."
//
// So the gate is ONE bit -- has_creds. `provisioned` NEVER decides whether a
// portal opens; it only selects WHICH entry reason is reported, purely for
// diagnostics:
//   !has_creds && !provisioned -> EV_ENTRY_FIRST_BOOT      true first boot
//   !has_creds &&  provisioned -> EV_ENTRY_DEPROV_ANOMALY  creds absent from
//                                  a board that HAD completed a validated
//                                  connect -- a genuine anomaly, logged
//                                  distinctly from a true first boot
//    has_creds (either provisioned value) -> no portal; normal STA connect,
//                                  wifi_reconn owns it
//
// A future reader may be tempted to "simplify" this into `return
// !provisioned;` -- don't: that collapses exactly the has-creds/provisioned
// distinction the bb_settings.h rationale above exists to prevent, and would
// strand the first-boot-after-portal-save board described there.
// ---------------------------------------------------------------------------

// Returns true and sets *out_event to the entry reason (EV_ENTRY_FIRST_BOOT
// or EV_ENTRY_DEPROV_ANOMALY) when a portal should open on boot; returns
// false (never touching *out_event) when creds are already present. NULL
// out_event is tolerated (result still correct) but the caller then has no
// way to learn which reason applied -- pass a real pointer whenever the
// caller cannot rule out !has_creds ahead of time. Pure: no ESP-IDF, no
// bb_settings call inside -- both bools come from the caller (the ESP-IDF
// shell reads bb_settings_wifi_has_creds()/bb_settings_wifi_provisioned_get()
// and passes the results in), so this is fully host-testable.
bool wifi_prov_entry_decision(bool has_creds, bool provisioned, wp_event_t *out_event);
