#pragma once
// Test-only hooks for bb_event and its platform ports. Gated on BB_EVENT_TESTING
// so production builds never compile against them. Tests include this header
// instead of reaching into bb_event_port.h or sibling component internals.
#ifdef BB_EVENT_TESTING

#include <stddef.h>
#include <time.h>
#include "bb_event_port.h"  // re-export queue entry typedef + dispatch decl for tests

// Reset bb_event global state so a test can start from a clean topic registry
// and re-run bb_event_init without leaking subscribers across cases.
void bb_event_reset_for_test(void);

// Reset the subscriber pool initialization flag so the next bb_event_init
// re-runs the pool guard (covers the second-init path in coverage).
void bb_event_reset_pool_for_test(void);

// Swap the malloc used by the host port queue allocation; NULL restores libc.
void bb_event_port_set_malloc(void *(*m)(size_t));

// Reset port-side state (queue, dispatcher thread, condvar) so tests can
// re-init the bus without leaking host resources.
void bb_event_port_reset_for_test(void);

// Delay the dispatcher thread's very first iteration by `ms` (before it ever
// touches the mutex/condvars), simulating a freshly-spawned dispatcher the
// OS hasn't scheduled yet. bb_event_port_reset_for_test resets this to 0, so
// it never leaks into a later test.
void bb_event_port_test_set_dispatcher_startup_delay_ms(uint32_t ms);

// Wall-clock duration (ms) of the most recent drain-wait segment inside
// bb_event_port_reset_for_test (the pthread_cond_timedwait loop only, not
// the surrounding pthread_join -- see bb_event_host.c for why the join
// alone can't be used to prove the drain-wait ran). -1 if no drain-wait ran
// on the last call.
long bb_event_port_test_get_last_drain_wait_ms(void);

// Exposes bb_event_port_reset_for_test's pure deadline-computation helper so
// tests can exercise the tv_nsec-overflow-normalization branch with crafted
// `now` values -- calling clock_gettime() directly from a test would make
// hitting that branch a wall-clock coin flip (whether "now" happens to fall
// in the back half of the current second), the same class of scheduling
// flakiness this file's drain-wait fix exists to eliminate.
struct timespec bb_event_port_test_compute_drain_deadline(struct timespec now, uint32_t timeout_ms);

// bb_event_common_dispatch and the bb_event_queue_entry_t typedef are
// re-exported via the bb_event_port.h include above for tests that exercise
// the dispatch path directly without going through bb_event_pump.

#endif
