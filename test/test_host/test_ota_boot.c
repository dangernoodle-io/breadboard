#include "unity.h"
#include "bb_ota_boot.h"

// The flag is persisted via the generic bb_nv API, which is a no-op stub on the
// host build (NVS is ESP-only) — so these exercise call-safety of the portable
// surface, not the round-trip (that is covered on-device). Mirrors the
// bb_ota_pull/push host-test pattern.

void test_ota_boot_pending_returns_bool(void)
{
    // Callable without crashing; result is whatever the host stub reports.
    bool p = bb_ota_boot_pending();
    TEST_ASSERT_TRUE(p == true || p == false);
}

void test_ota_boot_arm_callable(void)
{
    bb_ota_boot_arm();
    TEST_ASSERT_TRUE(true);
}

static void noop_progress(bb_ota_phase_t phase, int pct) { (void)phase; (void)pct; }

void test_ota_boot_progress_cb_registration(void)
{
    bb_ota_boot_set_progress_cb(noop_progress);
    bb_ota_boot_set_progress_cb(NULL);
    TEST_ASSERT_TRUE(true);
}
