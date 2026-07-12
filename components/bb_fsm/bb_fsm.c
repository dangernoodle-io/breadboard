// bb_fsm -- table-driven finite state machine primitive. See
// include/bb_fsm.h for the full public contract; this file is the sole
// implementation (no platform/ split -- pure logic, zero platform calls).
#include "bb_fsm.h"

// ---------------------------------------------------------------------------
// Pure helpers (no lock, no platform call -- host + device testable boundary)
// ---------------------------------------------------------------------------

static const bb_fsm_state_desc_t *find_state_desc(const bb_fsm_desc_t *desc, bb_fsm_state_t state)
{
    for (size_t i = 0; i < desc->state_count; i++) {
        if (desc->states[i].state == state) return &desc->states[i];
    }
    return NULL;
}

static void run_entry(bb_fsm_t *fsm, bb_fsm_state_t state)
{
    if (!fsm->desc->states) return;
    const bb_fsm_state_desc_t *sd = find_state_desc(fsm->desc, state);
    if (sd && sd->on_entry) sd->on_entry(fsm, fsm->desc->ctx, state);
}

static void run_exit(bb_fsm_t *fsm, bb_fsm_state_t state)
{
    if (!fsm->desc->states) return;
    const bb_fsm_state_desc_t *sd = find_state_desc(fsm->desc, state);
    if (sd && sd->on_exit) sd->on_exit(fsm, fsm->desc->ctx, state);
}

static void clear_timers(bb_fsm_t *fsm)
{
    for (size_t i = 0; i < BB_FSM_MAX_TIMERS; i++) {
        fsm->timers[i].armed = false;
    }
}

static const bb_fsm_row_t *find_row(const bb_fsm_desc_t *desc, bb_fsm_state_t current,
                                     bb_fsm_event_t event, void *evt_data)
{
    for (size_t i = 0; i < desc->row_count; i++) {
        const bb_fsm_row_t *row = &desc->rows[i];
        bool state_match = (row->state == current) || (row->state == BB_FSM_STATE_ANY);
        if (!state_match || row->event != event) continue;
        if (row->guard && !row->guard(desc->ctx, event, evt_data)) continue;
        return row;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_fsm_init(bb_fsm_t *fsm, const bb_fsm_desc_t *desc)
{
    if (!fsm || !desc) return BB_ERR_INVALID_ARG;
    if (!desc->rows && desc->row_count > 0) return BB_ERR_INVALID_ARG;
    if (!desc->states && desc->state_count > 0) return BB_ERR_INVALID_ARG;

    fsm->desc = desc;
    fsm->current = desc->initial;
    fsm->terminal = false;
    clear_timers(fsm);

    run_entry(fsm, fsm->current);
    return BB_OK;
}

bb_err_t bb_fsm_step(bb_fsm_t *fsm, bb_fsm_event_t event, void *evt_data)
{
    if (!fsm) return BB_ERR_INVALID_ARG;
    if (!fsm->desc) return BB_ERR_INVALID_STATE;
    if (fsm->terminal) return BB_ERR_INVALID_STATE;

    const bb_fsm_row_t *row = find_row(fsm->desc, fsm->current, event, evt_data);
    if (!row) return BB_ERR_NOT_FOUND;

    if (row->action) row->action(fsm, fsm->desc->ctx, event, evt_data);

    if (row->next == BB_FSM_STATE_SAME) {
        return BB_OK; // no exit/entry, timers untouched
    }
    if (row->next == BB_FSM_STATE_TERMINAL) {
        fsm->terminal = true; // no hooks
        clear_timers(fsm);    // latch is final -- a timer armed by this action must not survive
        return BB_OK;
    }

    bb_fsm_state_t old_state = fsm->current;
    run_exit(fsm, old_state);
    clear_timers(fsm);
    fsm->current = row->next;
    run_entry(fsm, fsm->current);
    return BB_OK;
}

bb_fsm_state_t bb_fsm_state(const bb_fsm_t *fsm)
{
    return fsm ? fsm->current : BB_FSM_STATE_ANY;
}

bool bb_fsm_is_terminal(const bb_fsm_t *fsm)
{
    return fsm ? fsm->terminal : false;
}

bb_err_t bb_fsm_arm_timer(bb_fsm_t *fsm, bb_fsm_event_t timeout_event, uint32_t ms)
{
    if (!fsm) return BB_ERR_INVALID_ARG;
    if (!fsm->desc) return BB_ERR_INVALID_STATE;

    int free_slot = -1;
    for (size_t i = 0; i < BB_FSM_MAX_TIMERS; i++) {
        if (fsm->timers[i].armed && fsm->timers[i].event == timeout_event) {
            if (ms == 0) {
                fsm->timers[i].armed = false;
            } else {
                fsm->timers[i].ms = ms;
            }
            return BB_OK;
        }
        if (!fsm->timers[i].armed && free_slot < 0) {
            free_slot = (int)i;
        }
    }

    if (ms == 0) return BB_OK; // nothing armed under this event -- no-op

    if (free_slot < 0) return BB_ERR_NO_SPACE;

    fsm->timers[free_slot].event = timeout_event;
    fsm->timers[free_slot].ms = ms;
    fsm->timers[free_slot].armed = true;
    return BB_OK;
}

void bb_fsm_disarm_timer(bb_fsm_t *fsm, bb_fsm_event_t timeout_event)
{
    if (!fsm) return;
    for (size_t i = 0; i < BB_FSM_MAX_TIMERS; i++) {
        if (fsm->timers[i].armed && fsm->timers[i].event == timeout_event) {
            fsm->timers[i].armed = false;
            return;
        }
    }
}

size_t bb_fsm_timer_count(const bb_fsm_t *fsm)
{
    if (!fsm) return 0;
    size_t count = 0;
    for (size_t i = 0; i < BB_FSM_MAX_TIMERS; i++) {
        if (fsm->timers[i].armed) count++;
    }
    return count;
}

bool bb_fsm_timer_at(const bb_fsm_t *fsm, size_t i, bb_fsm_event_t *event_out, uint32_t *ms_out)
{
    if (!fsm) return false;
    size_t seen = 0;
    for (size_t idx = 0; idx < BB_FSM_MAX_TIMERS; idx++) {
        if (!fsm->timers[idx].armed) continue;
        if (seen == i) {
            if (event_out) *event_out = fsm->timers[idx].event;
            if (ms_out) *ms_out = fsm->timers[idx].ms;
            return true;
        }
        seen++;
    }
    return false;
}
