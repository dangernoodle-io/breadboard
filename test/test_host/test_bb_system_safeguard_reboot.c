// Host tests for bb_system's WiFi safeguard-reboot facade (B1-790 slice,
// closing B1-1002/B1-1003/B1-884): the pure quadrant cases for
// bb_system_safeguard_reboot_should_increment (esp. the previously-unguarded
// unsynced+validated gap), bb_system_safeguard_reboot_allowed_at's
// synced-vs-unsynced source split (unsynced uses the boot-fail counter,
// ignoring the budget; synced uses the budget, ignoring the boot-fail
// counter), the cause->src mapping, and the account() orchestration
// (everything bb_system_safeguard_reboot does except the actual restart --
// exercised directly so host tests never hit exit(0)).
#include "unity.h"
#include "bb_system.h"
#include "bb_system_test.h"
#include "bb_storage.h"
#include "bb_storage_ram.h"
#include "bb_core.h"

static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_ram_test_reset();
    bb_storage_ram_register();
    bb_system_reboot_budget_reset_for_test();
    bb_system_boot_count_reset_for_test();
}

/* ---------------------------------------------------------------------------
 * bb_system_safeguard_reboot_should_increment -- all 4 quadrants.
 * ---------------------------------------------------------------------------*/

void test_bb_system_safeguard_reboot_should_increment_synced_validated_is_false(void)
{
    TEST_ASSERT_FALSE(bb_system_safeguard_reboot_should_increment(/*ota_validated=*/true, /*synced=*/true));
}

void test_bb_system_safeguard_reboot_should_increment_synced_unvalidated_is_true(void)
{
    TEST_ASSERT_TRUE(bb_system_safeguard_reboot_should_increment(false, true));
}

// The regression guard for the throttle gap this facade closes: pre-facade,
// an unsynced+validated device incremented neither the boot-fail counter
// nor was throttled by the budget (which no-ops when unsynced) -- nothing
// counted it. should_increment now returns true here.
void test_bb_system_safeguard_reboot_should_increment_unsynced_validated_is_true(void)
{
    TEST_ASSERT_TRUE(bb_system_safeguard_reboot_should_increment(true, false));
}

void test_bb_system_safeguard_reboot_should_increment_unsynced_unvalidated_is_true(void)
{
    TEST_ASSERT_TRUE(bb_system_safeguard_reboot_should_increment(false, false));
}

/* ---------------------------------------------------------------------------
 * bb_system_safeguard_reboot_src_for_cause
 * ---------------------------------------------------------------------------*/

void test_bb_system_safeguard_reboot_src_for_cause_wifi_safeguard(void)
{
    TEST_ASSERT_EQUAL(BB_RESET_SRC_WIFI_SAFEGUARD,
                       bb_system_safeguard_reboot_src_for_cause(BB_REBOOT_CAUSE_WIFI_SAFEGUARD));
}

void test_bb_system_safeguard_reboot_src_for_cause_egress_tier3(void)
{
    TEST_ASSERT_EQUAL(BB_RESET_SRC_EGRESS_TIER3,
                       bb_system_safeguard_reboot_src_for_cause(BB_REBOOT_CAUSE_EGRESS_TIER3));
}

void test_bb_system_safeguard_reboot_src_for_cause_invalid_is_unknown(void)
{
    TEST_ASSERT_EQUAL(BB_RESET_SRC_UNKNOWN,
                       bb_system_safeguard_reboot_src_for_cause(BB_REBOOT_CAUSE_COUNT));
}

/* ---------------------------------------------------------------------------
 * bb_system_safeguard_reboot_allowed_at -- unsynced uses the boot-fail
 * counter (ignoring the budget entirely); synced uses the budget (ignoring
 * the boot-fail counter entirely).
 * ---------------------------------------------------------------------------*/

// The exact bug regression guard: unsynced + boot-fail count already at/over
// threshold -> denied, even though the budget (which no-ops unsynced) would
// have said "allowed".
void test_bb_system_safeguard_reboot_allowed_at_unsynced_over_threshold_is_false(void)
{
    reset_all();
    for (uint8_t i = 0; i < BB_SYSTEM_BOOT_FAIL_THRESHOLD; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_system_boot_count_increment());
    }
    TEST_ASSERT_FALSE(bb_system_safeguard_reboot_allowed_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, false, 100000));
}

void test_bb_system_safeguard_reboot_allowed_at_unsynced_under_threshold_is_true(void)
{
    reset_all();
    TEST_ASSERT_TRUE(bb_system_safeguard_reboot_allowed_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, false, 100000));
}

// synced ignores the boot-fail counter entirely -- an exhausted boot-fail
// counter does not deny a synced decision (the budget is the sole gate).
void test_bb_system_safeguard_reboot_allowed_at_synced_ignores_boot_fail_counter(void)
{
    reset_all();
    for (uint8_t i = 0; i < BB_SYSTEM_BOOT_FAIL_THRESHOLD; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_system_boot_count_increment());
    }
    TEST_ASSERT_TRUE(bb_system_safeguard_reboot_allowed_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, true, 100000));
}

// synced + exhausted budget (cooldown just recorded) -> denied by the budget,
// independent of boot-fail count (left at 0 here).
void test_bb_system_safeguard_reboot_allowed_at_synced_budget_exhausted_is_false(void)
{
    reset_all();
    uint32_t now_s = 100000;
    bb_system_reboot_budget_record_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, true, now_s);
    TEST_ASSERT_FALSE(bb_system_safeguard_reboot_allowed_at(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, true, now_s + 1));
}

/* ---------------------------------------------------------------------------
 * bb_system_safeguard_reboot_account -- orchestration (increment + budget
 * record), no restart.
 * ---------------------------------------------------------------------------*/

void test_bb_system_safeguard_reboot_account_unsynced_unvalidated_increments(void)
{
    reset_all();
    bb_system_safeguard_reboot_account(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, /*ota_validated=*/false, /*synced=*/false);
    TEST_ASSERT_EQUAL_UINT8(1, bb_system_boot_count_get());
}

// The gap case: unsynced + validated must STILL increment.
void test_bb_system_safeguard_reboot_account_unsynced_validated_increments(void)
{
    reset_all();
    bb_system_safeguard_reboot_account(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, /*ota_validated=*/true, /*synced=*/false);
    TEST_ASSERT_EQUAL_UINT8(1, bb_system_boot_count_get());
}

void test_bb_system_safeguard_reboot_account_synced_validated_does_not_increment(void)
{
    reset_all();
    bb_system_safeguard_reboot_account(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, /*ota_validated=*/true, /*synced=*/true);
    TEST_ASSERT_EQUAL_UINT8(0, bb_system_boot_count_get());
}

// account() also calls through to bb_system_reboot_budget_record (the
// per-platform wrapper) unconditionally -- on host that wrapper itself
// hardcodes synced=false (bb_system_host.c), so it is always a no-op
// regardless of the synced arg passed to account(); that per-platform
// no-op behavior is already covered by test_bb_system_reboot_budget.c's
// test_bb_system_reboot_budget_record_is_noop_on_host. This just proves
// account() doesn't crash calling through it.
void test_bb_system_safeguard_reboot_account_calls_through_budget_record(void)
{
    reset_all();
    bb_system_safeguard_reboot_account(BB_REBOOT_CAUSE_WIFI_SAFEGUARD, false, true);
}

/* ---------------------------------------------------------------------------
 * bb_system_safeguard_reboot_allowed / bb_system_safeguard_reboot -- the
 * per-platform wrapper. On host this straight-lines to the unsynced branch
 * already covered above (bb_system_host.c: `_at(cause, false, 0U)`); the
 * restart-issuing bb_system_safeguard_reboot itself is not host-tested here
 * (it calls bb_system_restart_reason, whose host stub exits the process --
 * matches the precedent set by bb_system_restart_reason's own test
 * coverage).
 * ---------------------------------------------------------------------------*/

void test_bb_system_safeguard_reboot_allowed_is_true_on_host(void)
{
    reset_all();
    TEST_ASSERT_TRUE(bb_system_safeguard_reboot_allowed(BB_REBOOT_CAUSE_WIFI_SAFEGUARD));
}
