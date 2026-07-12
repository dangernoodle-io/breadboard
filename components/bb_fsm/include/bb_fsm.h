#pragma once

/**
 * @brief Table-driven finite state machine primitive: consumer-owned rows
 * (state, event, guard, action, next), entry/exit hooks, and a fixed-size
 * timer-arm seam for the shell to reconstruct real OS timers from. Pure
 * per-instance library -- no autoinit, no global state, no lock, embedded
 * by value in the consumer's own struct.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bb_core.h"   // bb_err_t

// ---------------------------------------------------------------------------
// Capacity constant (Kconfig bridge -- pattern from bb_sub.h / bb_cache.h /
// bb_clock.h). BB_FSM_MAX_TIMERS MUST be uniform project-wide: it sizes the
// embedded timers[] array below, so every translation unit must see the
// same value or sizeof(bb_fsm_t) diverges across TUs -- undefined behavior.
// Never shadow CONFIG_BB_FSM_MAX_TIMERS itself with a bare #ifndef.
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#  include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_FSM_MAX_TIMERS
#define BB_FSM_MAX_TIMERS CONFIG_BB_FSM_MAX_TIMERS
#endif
#ifndef BB_FSM_MAX_TIMERS
#define BB_FSM_MAX_TIMERS 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int16_t bb_fsm_state_t;
typedef int16_t bb_fsm_event_t;
typedef struct bb_fsm bb_fsm_t;   // forward decl (hooks reference it)

// Consumer state/event ids must be non-negative (0..32767); negative values
// are reserved for the ANY/SAME/TERMINAL/NONE sentinels below.
#define BB_FSM_STATE_ANY       ((bb_fsm_state_t)-1)   // row wildcard: matches any current state
#define BB_FSM_STATE_SAME      ((bb_fsm_state_t)-2)   // next: self-transition (action runs, NO exit/entry, timers kept)
#define BB_FSM_STATE_TERMINAL  ((bb_fsm_state_t)-3)   // next: latch terminal after action, timers cleared
#define BB_FSM_EVENT_NONE      ((bb_fsm_event_t)-1)

// GUARD is engine-pure: receives no fsm handle, so it cannot arm timers or
// mutate FSM state. It may read ctx but should not mutate it. NULL = always
// true.
typedef bool (*bb_fsm_guard_fn)(void *ctx, bb_fsm_event_t event, void *evt_data);
// ACTION owns ALL mutation/side-effects; gets the fsm handle so it may arm
// timers. NULL = no-op. Timers armed by an action are KEPT only on a SAME
// transition; on a concrete `next` the post-action clear discards them --
// arm in the destination state's on_entry instead (a TERMINAL `next` also
// clears). Not re-entrant -- an action must not call bb_fsm_step() on the
// same fsm (see bb_fsm_step()).
typedef void (*bb_fsm_action_fn)(bb_fsm_t *fsm, void *ctx, bb_fsm_event_t event, void *evt_data);
// ENTRY/EXIT per-state hook. Entry is the natural place to arm timers.
typedef void (*bb_fsm_hook_fn)(bb_fsm_t *fsm, void *ctx, bb_fsm_state_t state);

typedef struct {
    bb_fsm_state_t   state;   // match against current; BB_FSM_STATE_ANY = wildcard
    bb_fsm_event_t   event;
    bb_fsm_guard_fn  guard;   // NULL = always true
    bb_fsm_action_fn action;  // NULL = no-op
    bb_fsm_state_t   next;    // concrete state id | BB_FSM_STATE_SAME | BB_FSM_STATE_TERMINAL
} bb_fsm_row_t;

typedef struct {
    bb_fsm_state_t  state;
    bb_fsm_hook_fn  on_entry;
    bb_fsm_hook_fn  on_exit;
} bb_fsm_state_desc_t;

typedef struct {
    const bb_fsm_row_t        *rows;
    size_t                      row_count;
    const bb_fsm_state_desc_t *states;   // may be NULL/0 (no hooks)
    size_t                      state_count;
    bb_fsm_state_t              initial;
    void                       *ctx;     // consumer's own struct; opaque to bb_fsm
} bb_fsm_desc_t;

// Single-writer contract: an instance is not internally locked -- a
// consumer sharing one across tasks/ISRs must serialize its own calls
// (bb_fsm_step()/arm/disarm from a single owner, e.g. one task or under
// the consumer's own mutex).
struct bb_fsm {
    const bb_fsm_desc_t *desc;
    bb_fsm_state_t       current;
    bool                  terminal;
    struct {
        bb_fsm_event_t event;
        uint32_t       ms;
        bool           armed;
    } timers[BB_FSM_MAX_TIMERS];
};

// current = desc->initial, timers cleared, runs the initial state's
// on_entry (if states/hooks are provided). NULL fsm/desc, or a malformed
// desc (NULL rows with row_count > 0, NULL states with state_count > 0) --
// BB_ERR_INVALID_ARG.
bb_err_t bb_fsm_init(bb_fsm_t *fsm, const bb_fsm_desc_t *desc);

// Scans rows in table order for (state==current || state==ANY) &&
// event==ev; the first such row whose guard passes (or guard==NULL) wins.
// Runs its action, then resolves next: SAME (stay, no hooks, timers kept),
// TERMINAL (latch terminal, no hooks, timers cleared), or a concrete state
// (on_exit(old), timers cleared, current=next, on_entry(new)). Timers armed
// by the action are therefore KEPT only on a SAME transition -- a concrete
// `next` or TERMINAL discards them via the post-action clear; arm in the
// destination state's on_entry instead. No matching/passing row --
// BB_ERR_NOT_FOUND, state unchanged. Called on a terminal instance, or on
// a fsm never passed to bb_fsm_init() -- BB_ERR_INVALID_STATE, no-op. NULL
// fsm -- BB_ERR_INVALID_ARG. Not re-entrant: an action must not call
// bb_fsm_step() on the same fsm.
bb_err_t bb_fsm_step(bb_fsm_t *fsm, bb_fsm_event_t event, void *evt_data);

// NULL fsm returns BB_FSM_STATE_ANY as the null-handle sentinel -- do not
// mistake it for a real current state.
bb_fsm_state_t bb_fsm_state(const bb_fsm_t *fsm);
bool           bb_fsm_is_terminal(const bb_fsm_t *fsm);

// Timer arm seam -- call ONLY from an action/entry/exit hook (bb_fsm owns
// no real timer). timeout_event IS the timer id; re-arming an already-armed
// event overwrites its ms. ms==0 disarms. A NEW distinct event beyond
// BB_FSM_MAX_TIMERS armed slots -- BB_ERR_NO_SPACE. NULL fsm, or a fsm
// never passed to bb_fsm_init() -- BB_ERR_INVALID_ARG / BB_ERR_INVALID_STATE.
bb_err_t bb_fsm_arm_timer(bb_fsm_t *fsm, bb_fsm_event_t timeout_event, uint32_t ms);
void     bb_fsm_disarm_timer(bb_fsm_t *fsm, bb_fsm_event_t timeout_event);

// Shell-facing: after each step, reconstruct real OS timers to match the
// armed set, then on expiry call bb_fsm_step(fsm, timeout_event, NULL).
size_t bb_fsm_timer_count(const bb_fsm_t *fsm);
bool   bb_fsm_timer_at(const bb_fsm_t *fsm, size_t i, bb_fsm_event_t *event_out, uint32_t *ms_out);

#ifdef __cplusplus
}
#endif
