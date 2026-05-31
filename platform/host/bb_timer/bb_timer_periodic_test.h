#pragma once

#include "bb_timer.h"

/**
 * Host-only test hook: synchronously invoke the timer callback as if the
 * period elapsed. Only fires if the timer is currently running.
 * Only available when BB_TIMER_TESTING is defined.
 */
void bb_timer_periodic_fire_for_test(bb_periodic_timer_t t);
