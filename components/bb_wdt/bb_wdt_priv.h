#pragma once

// bb_wdt — private cross-TU seam between the portable core-claim bookkeeping
// (bb_wdt_core_claim.c) and each platform's reconfigure implementation
// (platform/espidf/bb_wdt/bb_wdt.c, platform/host/bb_wdt/bb_wdt.c). Not
// installed under include/ -- never exposed to consumers.

#include "bb_wdt.h"

// Reapply the current timeout (whatever bb_wdt_set_timeout() last set, or
// the platform default before the first call) with the current derived idle
// mask. Called by bb_wdt_claim_core()/bb_wdt_release_core() immediately
// after the claimed-core bitmask changes, so there is never a window where
// a claim is bookkept but not yet applied to the real (or stubbed)
// reconfigure call. Implemented once per platform TU.
void bb_wdt_priv_reapply(void);

#ifdef BB_WDT_TESTING
// Reset the claimed-core bitmask to 0. Test isolation only; called by the
// host backend's bb_wdt_test_reset().
void bb_wdt_priv_test_reset_claims(void);
#endif
