#include "unity.h"
#include "bb_settings_creds_boot_decide.h"

// bb_settings_creds_boot_decide is the pure heal-vs-seed DECISION extracted
// from bb_settings_creds_boot_init's #ifdef ESP_PLATFORM body
// (platform/host/bb_settings/bb_settings.c) -- the highest-risk branch in
// that file, since an inverted decision strands a board (heals when it
// should seed, or vice versa). Moved+renamed (B1-963/B1-708) from
// test_bb_nv_creds_boot_decide.c / bb_nv_creds_boot_decide, byte-for-byte
// identical policy. This covers all 4 input combinations against the real
// inline policy; the NVS/RTC-mirror I/O the caller performs for each action
// remains espidf-only and is NOT covered by these tests (see
// bb_settings.c's comment at the call site).

void test_bb_settings_creds_boot_decide_none_when_both_absent(void)
{
    // No live creds, no mirror -- nothing to heal, nothing to seed.
    TEST_ASSERT_EQUAL(BB_SETTINGS_CREDS_BOOT_NONE, bb_settings_creds_boot_decide(false, false));
}

void test_bb_settings_creds_boot_decide_seed_when_live_creds_and_no_mirror(void)
{
    // Live creds present, mirror empty/invalid -- arm the recovery net.
    TEST_ASSERT_EQUAL(BB_SETTINGS_CREDS_BOOT_SEED, bb_settings_creds_boot_decide(true, false));
}

void test_bb_settings_creds_boot_decide_heal_when_no_live_creds_and_valid_mirror(void)
{
    // NVS erased/empty but the RTC mirror still holds creds -- restore.
    TEST_ASSERT_EQUAL(BB_SETTINGS_CREDS_BOOT_HEAL, bb_settings_creds_boot_decide(false, true));
}

void test_bb_settings_creds_boot_decide_none_when_both_present(void)
{
    // Live creds present AND mirror already valid -- no heal needed (creds
    // are live), no seed needed (mirror already armed); never overwrite a
    // valid mirror that may carry in-flight pending-try state.
    TEST_ASSERT_EQUAL(BB_SETTINGS_CREDS_BOOT_NONE, bb_settings_creds_boot_decide(true, true));
}
