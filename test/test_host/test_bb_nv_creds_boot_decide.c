#include "unity.h"
#include "bb_nv_creds_boot_decide.h"

// bb_nv_creds_boot_decide is the pure heal-vs-seed DECISION extracted from
// bb_nv_config_init's #ifdef ESP_PLATFORM body (platform/espidf/bb_nv/
// bb_nv.c) -- the highest-risk branch in that file, since an inverted
// decision strands a board (heals when it should seed, or vice versa). This
// covers all 4 input combinations against the real inline policy; the
// NVS/RTC-mirror I/O the caller performs for each action remains
// espidf-only and is NOT covered by these tests (see bb_nv.c's comment at
// the call site).

void test_bb_nv_creds_boot_decide_none_when_both_absent(void)
{
    // No live creds, no mirror -- nothing to heal, nothing to seed.
    TEST_ASSERT_EQUAL(BB_NV_BOOT_NONE, bb_nv_creds_boot_decide(false, false));
}

void test_bb_nv_creds_boot_decide_seed_when_live_creds_and_no_mirror(void)
{
    // Live creds present, mirror empty/invalid -- arm the recovery net.
    TEST_ASSERT_EQUAL(BB_NV_BOOT_SEED, bb_nv_creds_boot_decide(true, false));
}

void test_bb_nv_creds_boot_decide_heal_when_no_live_creds_and_valid_mirror(void)
{
    // NVS erased/empty but the RTC mirror still holds creds -- restore.
    TEST_ASSERT_EQUAL(BB_NV_BOOT_HEAL, bb_nv_creds_boot_decide(false, true));
}

void test_bb_nv_creds_boot_decide_none_when_both_present(void)
{
    // Live creds present AND mirror already valid -- no heal needed (creds
    // are live), no seed needed (mirror already armed); never overwrite a
    // valid mirror that may carry in-flight pending-try state.
    TEST_ASSERT_EQUAL(BB_NV_BOOT_NONE, bb_nv_creds_boot_decide(true, true));
}
