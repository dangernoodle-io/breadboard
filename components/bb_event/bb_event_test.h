#pragma once
// Test-only hooks for bb_event and its platform ports. Gated on BB_EVENT_TESTING
// so production builds never compile against them. Tests include this header
// instead of reaching into bb_event_port.h or sibling component internals.
#ifdef BB_EVENT_TESTING

#include <stddef.h>
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

// bb_event_common_dispatch and the bb_event_queue_entry_t typedef are
// re-exported via the bb_event_port.h include above for tests that exercise
// the dispatch path directly without going through bb_event_pump.

#endif
