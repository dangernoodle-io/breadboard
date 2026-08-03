#include "bb_wdt.h"
#include "bb_wdt_priv.h"

#ifdef BB_WDT_TESTING
#include "bb_wdt_test.h"

static int   s_feed_count             = 0;
static int   s_subscribe_count        = 0;
static int   s_unsub_count            = 0;
static void *s_last_subscribe_handle  = NULL;
static void *s_last_unsubscribe_handle = NULL;
static uint32_t s_last_reconfigured_mask = 0;
// Simulated core count for bb_wdt_clamp_core_mask() -- mirrors the ESP-IDF
// backend's configNUMBER_OF_CORES argument. Defaults to 2 (dual-core) so
// existing dual-core-shaped tests are unaffected; a test sets this to 1 to
// reproduce a unicore (esp32-s2/-c3) target's clamp behavior.
static int s_num_cores = 2;

void bb_wdt_test_reset(void)
{
    s_feed_count              = 0;
    s_subscribe_count         = 0;
    s_unsub_count             = 0;
    s_last_subscribe_handle   = NULL;
    s_last_unsubscribe_handle = NULL;
    s_last_reconfigured_mask  = 0;
    s_num_cores               = 2;
    bb_wdt_priv_test_reset_claims();
}

int bb_wdt_test_feed_count(void)      { return s_feed_count; }
int bb_wdt_test_subscribe_count(void) { return s_subscribe_count; }
int bb_wdt_test_unsubscribe_count(void) { return s_unsub_count; }
void *bb_wdt_test_last_subscribe_handle(void)   { return s_last_subscribe_handle; }
void *bb_wdt_test_last_unsubscribe_handle(void) { return s_last_unsubscribe_handle; }
uint32_t bb_wdt_test_last_reconfigured_mask(void) { return s_last_reconfigured_mask; }
void bb_wdt_test_set_num_cores(int num_cores) { s_num_cores = num_cores; }
#endif /* BB_WDT_TESTING */

// Host has no Kconfig-derived idle-check default -- excused_seed 0U is the
// EXCUSED-polarity equivalent of "everything monitored by default" (the
// same result a default y/y Kconfig on the ESP-IDF backend inverts to --
// see bb_wdt.h's "Polarity hardening" note, B1-1408). SHARED reconfigure
// path -- both bb_wdt_set_timeout() and bb_wdt_priv_reapply() (claim/
// release) go through this single function, mirroring the ESP-IDF backend's
// do_reconfigure(), so a claim can never be silently clobbered by a later
// bb_wdt_set_timeout().
//
// The excused mask is clamped via bb_wdt_clamp_core_mask() exactly like the
// ESP-IDF backend, against the BB_WDT_TESTING-only simulated core count
// (s_num_cores, default 2) -- this is what makes the unicore
// out-of-range-claim behavior host-testable at all (see
// bb_wdt_test_set_num_cores()); the real ESP-IDF backend clamps against
// configNUMBER_OF_CORES instead. It is then converted back to MONITORED
// polarity exactly once via bb_wdt_invert_core_mask(), mirroring the
// ESP-IDF backend's single conversion immediately before writing
// esp_task_wdt_config_t.idle_core_mask -- s_last_reconfigured_mask is the
// host-observable stand-in for that same value.
static void do_reconfigure(uint32_t timeout_s)
{
    (void)timeout_s;
    bb_wdt_excused_mask_t excused_seed = { .bits = 0U };
    // A claim IS excused-polarity by contract (see bb_wdt.h's "Polarity
    // hardening" note) -- named conversion of the bare uint32_t
    // bb_wdt_claimed_core_mask() returns (kept bare there since it is also
    // consumed as a raw ownership bitmask outside bb_wdt).
    bb_wdt_excused_mask_t claimed_excused = { .bits = bb_wdt_claimed_core_mask() };
    bb_wdt_excused_mask_t excused =
        bb_wdt_derive_excused_mask(excused_seed, claimed_excused);
#ifdef BB_WDT_TESTING
    bb_wdt_excused_mask_t clamped = bb_wdt_clamp_core_mask(excused, s_num_cores);
    s_last_reconfigured_mask = bb_wdt_invert_core_mask(clamped.bits, s_num_cores);
#else
    (void)excused;
#endif
}

void bb_wdt_set_timeout(uint32_t timeout_s)
{
    do_reconfigure(timeout_s);
}

void bb_wdt_priv_reapply(void)
{
    do_reconfigure(0U);
}

void bb_wdt_extend_begin(uint32_t extended_s)
{
    (void)extended_s;
}

void bb_wdt_extend_end(void)
{
}

bb_err_t bb_wdt_task_subscribe(void)
{
    return bb_wdt_task_subscribe_handle(NULL);
}

bb_err_t bb_wdt_task_unsubscribe(void)
{
    return bb_wdt_task_unsubscribe_handle(NULL);
}

bb_err_t bb_wdt_task_subscribe_handle(void *handle)
{
#ifdef BB_WDT_TESTING
    s_subscribe_count++;
    s_last_subscribe_handle = handle;
#else
    (void)handle;
#endif
    return BB_OK;
}

bb_err_t bb_wdt_task_unsubscribe_handle(void *handle)
{
#ifdef BB_WDT_TESTING
    s_unsub_count++;
    s_last_unsubscribe_handle = handle;
#else
    (void)handle;
#endif
    return BB_OK;
}

void bb_wdt_task_feed(void)
{
#ifdef BB_WDT_TESTING
    s_feed_count++;
#endif
}
