#include "bb_mdns_lifecycle.h"

void bb_mdns_lifecycle_reset(bb_mdns_lifecycle_state_t *st)
{
    if (!st) return;
    st->started = false;
    st->announce_dirty = false;
}

/* Adapter contract: caller must populate all fn pointers. */
bb_mdns_lifecycle_result_t bb_mdns_lifecycle_start(bb_mdns_lifecycle_state_t *st, const bb_mdns_lifecycle_adapter_t *a)
{
    if (!st || !a) {
        return BB_MDNS_LC_INVALID_ARG;
    }

    if (st->started) {
        return BB_MDNS_LC_ALREADY_STARTED;
    }

    if (a->mdns_init() != 0) {
        return BB_MDNS_LC_INIT_FAILED;
    }

    st->started = true;

    /* If announce was pending before start, flush it now. */
    if (st->announce_dirty) {
        a->mdns_apply_announce();
        st->announce_dirty = false;
    }

    return BB_MDNS_LC_OK;
}

bb_mdns_lifecycle_result_t bb_mdns_lifecycle_stop(bb_mdns_lifecycle_state_t *st, const bb_mdns_lifecycle_adapter_t *a)
{
    if (!st || !a) {
        return BB_MDNS_LC_INVALID_ARG;
    }

    if (!st->started) {
        return BB_MDNS_LC_NOT_STARTED;
    }

    /* Send bye before free (PR #77 order). */
    a->mdns_send_bye();
    a->mdns_free();

    st->started = false;
    /* Leave announce_dirty alone so a future restart re-announces. */

    return BB_MDNS_LC_OK;
}

bb_mdns_lifecycle_result_t bb_mdns_lifecycle_announce(bb_mdns_lifecycle_state_t *st, const bb_mdns_lifecycle_adapter_t *a)
{
    if (!st || !a) {
        return BB_MDNS_LC_INVALID_ARG;
    }

    if (!st->started) {
        st->announce_dirty = true;
        return BB_MDNS_LC_NOT_STARTED;
    }

    a->mdns_apply_announce();
    st->announce_dirty = false;

    return BB_MDNS_LC_OK;
}

void bb_mdns_lifecycle_mark_dirty(bb_mdns_lifecycle_state_t *st)
{
    if (st) {
        st->announce_dirty = true;
    }
}

bool bb_mdns_lifecycle_is_started(const bb_mdns_lifecycle_state_t *st)
{
    if (!st) return false;
    return st->started;
}
