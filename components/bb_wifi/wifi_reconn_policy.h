#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bb_wifi.h"
#include "bb_fsm.h"

// Breadboard sentinel reason codes injected into reason_histogram — aliases
// of the public BB_WIFI_REASON_BB_* constants (bb_wifi.h) so the numeric
// values (wire-visible in GET /api/diag/net) have a single source of truth.
// 99/100/101 are free in esp_wifi_types.h (reasons: 1-24, 53-67, 200-208)
// and fit in uint8_t (< 256, the histogram size).
#define WIFI_REASON_BB_LOST_IP        BB_WIFI_REASON_BB_LOST_IP
#define WIFI_REASON_BB_EGRESS_DEAD    BB_WIFI_REASON_BB_EGRESS_DEAD
#define WIFI_REASON_BB_NO_IP_WATCHDOG BB_WIFI_REASON_BB_NO_IP_WATCHDOG

// --- Kconfig bridge (CONFIG_BB_WIFI_RECONN_* -> WIFI_RECONN_*) ---
// Never a bare #ifndef alongside the CONFIG_ symbol itself -- that shadows
// the generated Kconfig value and silently makes the knob inert.
#ifdef ESP_PLATFORM
#  include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS
#define WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS CONFIG_BB_WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS
#endif
#ifndef WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS
#define WIFI_RECONN_HANDSHAKE_BACKOFF_TIER2_MS 10000
#endif

#ifdef CONFIG_BB_WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS
#define WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS CONFIG_BB_WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS
#endif
#ifndef WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS
#define WIFI_RECONN_HANDSHAKE_BACKOFF_TIER3_MS 30000
#endif

#ifdef CONFIG_BB_WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS
#define WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS CONFIG_BB_WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS
#endif
#ifndef WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS
#define WIFI_RECONN_GENERIC_BACKOFF_PAUSE_MS 5000
#endif

#ifdef CONFIG_BB_WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT
#define WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT CONFIG_BB_WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT
#endif
#ifndef WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT
#define WIFI_RECONN_GENERIC_FAST_RETRY_LIMIT 10
#endif

// SLOW backoff tier (PR7, B1-994/B1-806): AUTH_FAIL/NO_AP_FOUND disconnects
// -- bad credentials or an absent AP -- never hot-retry, even on the FIRST
// occurrence. Two never-decaying steps (counter only resets on got_ip/reset,
// same shape as the handshake/generic ladders); see compute_backoff_ms.
#ifdef CONFIG_BB_WIFI_RECONN_SLOW_TIER1_LIMIT
#define WIFI_RECONN_SLOW_TIER1_LIMIT CONFIG_BB_WIFI_RECONN_SLOW_TIER1_LIMIT
#endif
#ifndef WIFI_RECONN_SLOW_TIER1_LIMIT
#define WIFI_RECONN_SLOW_TIER1_LIMIT 3
#endif

#ifdef CONFIG_BB_WIFI_RECONN_SLOW_BACKOFF_TIER1_MS
#define WIFI_RECONN_SLOW_BACKOFF_TIER1_MS CONFIG_BB_WIFI_RECONN_SLOW_BACKOFF_TIER1_MS
#endif
#ifndef WIFI_RECONN_SLOW_BACKOFF_TIER1_MS
#define WIFI_RECONN_SLOW_BACKOFF_TIER1_MS 30000
#endif

#ifdef CONFIG_BB_WIFI_RECONN_SLOW_BACKOFF_TIER2_MS
#define WIFI_RECONN_SLOW_BACKOFF_TIER2_MS CONFIG_BB_WIFI_RECONN_SLOW_BACKOFF_TIER2_MS
#endif
#ifndef WIFI_RECONN_SLOW_BACKOFF_TIER2_MS
#define WIFI_RECONN_SLOW_BACKOFF_TIER2_MS 120000
#endif

#ifdef CONFIG_BB_WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT
#define WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT CONFIG_BB_WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT
#endif
#ifndef WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT
#define WIFI_RECONN_HANDSHAKE_FAST_RETRY_LIMIT 3
#endif

#ifdef CONFIG_BB_WIFI_RECONN_HANDSHAKE_TIER2_LIMIT
#define WIFI_RECONN_HANDSHAKE_TIER2_LIMIT CONFIG_BB_WIFI_RECONN_HANDSHAKE_TIER2_LIMIT
#endif
#ifndef WIFI_RECONN_HANDSHAKE_TIER2_LIMIT
#define WIFI_RECONN_HANDSHAKE_TIER2_LIMIT 6
#endif

#ifdef CONFIG_BB_WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S
#define WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S CONFIG_BB_WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S
#endif
#ifndef WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S
#define WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S 300
#endif
#define WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US  ((int64_t)WIFI_RECONN_PERSISTENT_FAIL_WINDOW_S * 1000000LL)

// FSM WR_CONNECTING watchdog timeout (ms) -- moved here (from the ESP-IDF-only
// platform/espidf/bb_wifi/wifi_reconn.h) so the FSM table's WR_CONNECTING
// on_entry hook, which arms this timeout, lives in this host-compilable TU
// (R8/B1-805 slice 1a).
#ifdef CONFIG_BB_WIFI_RECONN_CONNECTING_TIMEOUT_MS
#define WIFI_RECONN_CONNECTING_TIMEOUT_MS CONFIG_BB_WIFI_RECONN_CONNECTING_TIMEOUT_MS
#endif
#ifndef WIFI_RECONN_CONNECTING_TIMEOUT_MS
#define WIFI_RECONN_CONNECTING_TIMEOUT_MS 30000U
#endif

typedef struct {
    int      handshake_fail_count;
    int      generic_fail_count;
    int      slow_fail_count; // AUTH_FAIL/NO_AP_FOUND bucket (PR7, B1-994/B1-806)
    int64_t  first_fail_us;
    int      retry_count;
    bb_wifi_disc_reason_t last_reason;
    int64_t  last_disconnect_us;
    uint16_t reason_histogram[BB_WIFI_DISC_COUNT];
    uint32_t lost_ip_count;    // times lost IP while associated
    int64_t  last_lost_ip_us;  // timestamp of last lost-IP event
    uint8_t  egress_fail_streak; // consecutive egress-probe failures below threshold
    uint32_t egress_dead_count;  // times egress declared dead (streak hit threshold)
    int64_t  last_egress_dead_us; // timestamp of last egress-dead event
} wifi_reconn_state_t;

// Adapter (R3, B1-805 slice 1a): every side-effecting call an FSM action or
// hook makes goes THROUGH one of these pointers -- zero bare esp_wifi_*/
// bb_system_*/reboot calls in the FSM logic below, so the host test fixture
// (a fully-populated fake) reaches every branch. Real wiring: a single
// file-static instance in platform/espidf/bb_wifi/wifi_reconn.c. All
// pointers are a required part of the contract (not optionally NULL) --
// callers populate every field.
typedef struct {
    int64_t (*now_us)(void);                  // existing (bb_timer_now_us)
    void    (*connect_fn)(void);              // esp_wifi_connect()
    void    (*disconnect_fn)(void);           // esp_wifi_disconnect()
    // No restart_sta_fn: no FSM row/action calls bb_wifi_restart_sta() in
    // this slice (review fix [MEDIUM], B1-805 slice 1a) -- reserved for
    // slice 1b, which reintroduces it alongside the inactive-time/egress-
    // recovery restart path (see the CONFIG_BB_WIFI_INACTIVE_TIME_ENABLE and
    // CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE compile-time tripwires in
    // platform/espidf/bb_wifi/wifi_reconn.c).
    // bb_system safeguard-reboot facade (B1-790 slice) -- collapses the
    // former 5 reboot hooks (budget_allows_fn/budget_record_fn/
    // boot_fail_over_fn/boot_count_increment_fn/ota_validated_fn) into these
    // 2. reboot_allowed_fn is the combined budget+boot-fail-throttle
    // decision (bb_system_safeguard_reboot_allowed); reboot_fn now also owns
    // the accounting (boot-fail bump + budget record) that used to be
    // spread across the other 4 hooks (bb_system_safeguard_reboot).
    bool    (*reboot_allowed_fn)(void);       // bb_system_safeguard_reboot_allowed(WIFI_SAFEGUARD)
    void    (*reboot_fn)(const char *detail); // bb_system_safeguard_reboot(WIFI_SAFEGUARD, ota_validated, detail)
    // Not part of the architect brief's esp_wifi_*/bb_system_*/reboot
    // enumeration, but required to keep the REBOOT_DENIED emit (R14) pure
    // and host-testable: bb_wifi_publish_net_event() does I/O (builds the ip
    // string, invokes the generic emit slot) and cannot be called bare from
    // this host-compiled TU.
    void    (*emit_net_event_fn)(bb_wifi_net_event_t evt, bb_wifi_disc_reason_t reason);
} wifi_reconn_adapter_t;

typedef enum {
    WIFI_RECONN_ACTION_NONE,
    WIFI_RECONN_ACTION_RECONNECT_NOW,
    WIFI_RECONN_ACTION_SCHEDULE_BACKOFF,
    WIFI_RECONN_ACTION_REBOOT,
} wifi_reconn_action_t;

// Reset all counters to zero.
void wifi_reconn_state_reset(wifi_reconn_state_t *st);

// Policy decision on disconnect. Caller passes the already-mapped portable
// bb_wifi_disc_reason_t (see bb_wifi_map_esp_reason/bb_wifi_map_wl_status)
// so this module stays free of any backend-specific reason-code type.
// Returns action enum; if SCHEDULE_BACKOFF, populates *backoff_ms_out.
wifi_reconn_action_t wifi_reconn_policy_on_disconnect(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a,
    bb_wifi_disc_reason_t reason, uint32_t *backoff_ms_out);

// Reset counters on successful IP acquisition.
void wifi_reconn_policy_on_got_ip(wifi_reconn_state_t *st);

// Return true when the board is L2-associated but has no DHCP IP — the zombie state.
bool wifi_reconn_should_reconnect_no_ip(bool associated, bool has_ip);

// Record a lost-IP event in policy state (bumps lost_ip_count, last_lost_ip_us,
// reason_histogram[WIFI_REASON_BB_LOST_IP]). Guards null args.
void wifi_reconn_policy_on_lost_ip(wifi_reconn_state_t *st, const wifi_reconn_adapter_t *ad);

// Policy decision when the egress probe finds the gateway unreachable.
// If reachable: resets egress_fail_streak, returns WIFI_RECONN_ACTION_NONE.
// If unreachable: increments egress_fail_streak; once >= fail_threshold bumps
// egress_dead_count, arms first_fail_us, records last_egress_dead_us,
// increments reason_histogram[WIFI_REASON_BB_EGRESS_DEAD], resets streak,
// returns WIFI_RECONN_ACTION_RECONNECT_NOW. Guards null args.
wifi_reconn_action_t wifi_reconn_policy_on_egress_probe(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *ad,
    bool reachable, int fail_threshold);

// Policy decision when a connect attempt stalls (no GOT_IP or DISCONNECT
// within the connecting watchdog window). Mirrors on_disconnect escalation:
// bumps generic_fail_count, sets first_fail_us if 0, increments retry_count.
// Returns WIFI_RECONN_ACTION_REBOOT after WIFI_RECONN_PERSISTENT_FAIL_WINDOW_US;
// otherwise applies the same progressive generic backoff as on_disconnect
// (RECONNECT_NOW within GENERIC_FAST_RETRY_LIMIT, SCHEDULE_BACKOFF beyond it).
wifi_reconn_action_t wifi_reconn_policy_on_connect_timeout(
    wifi_reconn_state_t *st, const wifi_reconn_adapter_t *a,
    uint32_t *backoff_ms_out);

// ---------------------------------------------------------------------------
// bb_fsm rebuild (B1-805 slice 1a) -- table-driven reconnect FSM. Host-
// compilable (bb_event-free, bb_fsm is a pure per-instance library); the
// ESP-IDF shell (platform/espidf/bb_wifi/wifi_reconn.c) embeds a bb_fsm_t
// built from wifi_reconn_fsm_desc_init() and drives it via its own
// FreeRTOS queue/timer loop. Guards/actions/hooks are file-static in
// wifi_reconn_policy.c; only the enums, ctx struct, and desc-init entry
// point are public.
// ---------------------------------------------------------------------------

// FSM states (bb_fsm.h reserves negative ids for its ANY/SAME/TERMINAL
// sentinels -- these must stay non-negative).
//   WR_NO_CREDS        passive/parked; no timer armed, only EV_CREDS_ARRIVED
//                       moves it (see the no-creds safety invariant below).
//   WR_CONNECTING       esp_wifi_connect() issued; CONNECTING watchdog armed.
//   WR_CONNECTED        GOT_IP acquired; idle, no timer.
//   WR_BACKOFF          backoff timer armed; expiry -> WR_CONNECTING.
//   WR_ESCALATE_REBOOT  reached ONLY on an allowed escalation (R14
//                       guard-placement) -- on_entry reboots unconditionally.
//   WR_LEFT             parked on an ASSOC_LEAVE disconnect (PR7, B1-994/
//                       B1-806) -- no timer armed, only EV_RECONNECT_REQUESTED
//                       moves it (same terminal-until-resume shape as
//                       WR_NO_CREDS). ASSOC_LEAVE means "stop auto-retrying,"
//                       not "reboot" -- a repeated kick/resume/kick loop
//                       deliberately never reaches the persistent-fail-window
//                       reboot escalation while parked here; this is an
//                       intentional scope boundary, revisit only on fleet
//                       evidence.
typedef enum {
    WR_NO_CREDS = 0,
    WR_CONNECTING = 1,
    WR_CONNECTED = 2,
    WR_BACKOFF = 3,
    WR_ESCALATE_REBOOT = 4,
    WR_LEFT = 5,
} wr_state_t;

// FSM events.
//   EV_STA_CONNECTED       WIFI_EVENT_STA_CONNECTED (assoc; not yet IP).
//   EV_GOT_IP              IP_EVENT_STA_GOT_IP.
//   EV_STA_DISCONNECTED    WIFI_EVENT_STA_DISCONNECTED (real, not
//                          self-induced); evt_data points at a uint8_t esp
//                          reason code, mapped inside the action via
//                          bb_wifi_map_esp_reason (R13, numeric wire values).
//   EV_CONNECTING_TIMEOUT  CONNECTING watchdog fired (shell timer).
//   EV_BACKOFF_TIMEOUT     backoff timer expired (shell timer).
//   EV_CREDS_ARRIVED       provisioning wrote creds (WR_NO_CREDS ->
//                          WR_CONNECTING); wiring who posts this is out of
//                          scope for slice 1a (only the table row is
//                          required) -- see B1-805.
//   EV_RECONNECT_REQUESTED resumes from WR_LEFT (PR7, B1-994/B1-806); ships
//                          UNWIRED, same precedent as EV_CREDS_ARRIVED --
//                          only the table row is required, no production
//                          poster in this PR.
typedef enum {
    EV_STA_CONNECTED = 0,
    EV_GOT_IP = 1,
    EV_STA_DISCONNECTED = 2,
    EV_CONNECTING_TIMEOUT = 3,
    EV_BACKOFF_TIMEOUT = 4,
    EV_CREDS_ARRIVED = 5,
    EV_RECONNECT_REQUESTED = 6,
} wr_event_t;

// Embedded-by-value FSM context, zero heap, single-writer (the reconn task
// is the sole bb_fsm_step()/arm/disarm caller -- non-reentrant, never call
// bb_fsm_step() from within a guard/action/hook on this instance).
//   fsm                 embedded bb_fsm_t (timers[BB_FSM_MAX_TIMERS==1]).
//   desc                the bb_fsm_desc_t bb_fsm_init()/fsm.desc POINTS AT --
//                       bb_fsm does not copy the desc it's given (see
//                       bb_fsm_init), so it MUST outlive the fsm; embedding
//                       it here (same lifetime as fsm/ctx) rather than a
//                       caller-local stack variable is load-bearing, not
//                       style -- a local desc going out of scope after
//                       wifi_reconn_fsm_desc_init() returns is a dangling-
//                       pointer bug caught in this slice's own host tests.
//   policy              existing counters (wifi_reconn_state_t, unchanged).
//   adapter             the fn-pointer table above; const, file-static.
//   pending_backoff_ms  set by a backoff-scheduling action, read by
//                       WR_BACKOFF's on_entry.
//   self_disconnect     migrated from the old file-static s_self_disconnect;
//                       read/cleared as an absorb-check by the shell's
//                       disconnect NOTIFIER (before enqueue -- not inside the
//                       FSM itself), set by act_timeout_reattempt (row 8,
//                       runs on the reconn task) and
//                       wifi_reconn_absorb_next_disconnect() (callable from
//                       an arbitrary caller task, e.g. bb_wifi_restart_sta()).
//                       The notifier that reads/clears it runs on the
//                       ESP-IDF default event-loop task -- a THIRD, distinct
//                       FreeRTOS task from either writer. esp_wifi_disconnect()
//                       does NOT emit WIFI_EVENT_STA_DISCONNECTED inline
//                       before returning -- it's dispatched asynchronously via
//                       the default event-loop task -- so there is no
//                       run-to-completion ordering between the writer(s) and
//                       reader here; `volatile` is load-bearing for cross-
//                       task visibility, not style (matches the pre-rebuild
//                       `static volatile bool s_self_disconnect`).
typedef struct {
    bb_fsm_t                     fsm;
    bb_fsm_desc_t                desc;
    wifi_reconn_state_t          policy;
    const wifi_reconn_adapter_t *adapter;
    uint32_t                     pending_backoff_ms;
    volatile bool                self_disconnect;
} wifi_reconn_ctx_t;

// Populate ctx->desc for the wifi reconnect FSM (table + state hooks are
// file-static const in wifi_reconn_policy.c) and call bb_fsm_init(&ctx->fsm,
// &ctx->desc). `initial` is WR_CONNECTING (creds present at boot) or
// WR_NO_CREDS (parked -- see the no-creds safety invariant), decided by the
// caller (bb_wifi_autoinit) BEFORE calling this. ctx->adapter MUST already
// be set (every guard/action/hook dereferences it). Returns bb_fsm_init's
// result.
bb_err_t wifi_reconn_fsm_init(wifi_reconn_ctx_t *ctx, bb_fsm_state_t initial);
