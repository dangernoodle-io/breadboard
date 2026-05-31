#pragma once

#include "bb_timer.h"

/**
 * Host-only test hook: synchronously invoke the timer callback as if the
 * delay elapsed. Fires only if the timer is currently armed; sets armed=false
 * first (one-shot semantics: fires once until re-armed). A second call after
 * firing is a no-op.
 * Only available when BB_TIMER_TESTING is defined.
 */
void bb_timer_oneshot_fire_for_test(bb_oneshot_timer_t t);
