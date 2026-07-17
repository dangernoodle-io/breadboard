#pragma once

// bb_lifecycle — private cross-TU seam between the always-built sync core
// (bb_lifecycle.c) and the optional async substrate (bb_lifecycle_async.c,
// B1-1034). Not installed under include/ -- never exposed to consumers; the
// two .c files in this component directory include it via a relative path.

#include "bb_lifecycle.h"
#include <stdbool.h>

// Register an observer slot with the given async flag, under s_lock, using
// the SAME append-only/bump-last publish invariant bb_lifecycle_observe()
// has always used. bb_lifecycle_observe() wraps this with async=false (its
// documented behavior is otherwise UNCHANGED); bb_lifecycle_observe_async()
// (bb_lifecycle_async.c) wraps it with async=true after its own lazy
// queue/task init succeeds. NULL cb -> BB_ERR_INVALID_ARG; table full ->
// BB_ERR_NO_SPACE.
bb_err_t bb_lifecycle_priv_observe_slot(bb_lifecycle_observer_fn cb, void *user, bool async);

// Invoke every async-flagged observer slot with `evt` (a registry READ only
// -- no queue, no task, no lock re-entry into a mutator). Called by the
// async drain task after bb_bqueue_receive(), and directly by the
// BB_LIFECYCLE_TESTING drain-dispatch test hook -- one code path, no mirror.
// A no-op stub when CONFIG_BB_LIFECYCLE_ASYNC=n (unreachable there -- no
// async slot can ever be registered in that build).
void bb_lifecycle_priv_invoke_async_slots(const bb_lifecycle_event_t *evt);

// Called by notify_all() (bb_lifecycle.c) at most once per real transition,
// IFF at least one async-flagged slot is currently registered. Enqueues
// `evt` onto the shared async queue with a non-blocking (timeout_ms=0) send;
// a full queue drops (rate-limited warn) and relies on bb_bqueue_dropped()
// for the counter. A no-op stub when CONFIG_BB_LIFECYCLE_ASYNC=n (also
// unreachable there for the same reason as above) -- bb_lifecycle.c calls
// this unconditionally so it never needs to know whether the gate is on.
void bb_lifecycle_priv_async_notify(const bb_lifecycle_event_t *evt);
