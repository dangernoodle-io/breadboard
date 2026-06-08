// Tests for bb_ota_led — lifecycle routing + restore-on-terminal contract.
#include "unity.h"
#include "bb_ota_led.h"

static int s_updating_calls;
static int s_success_calls;
static int s_restore_calls;
static int s_last_pct;
static void *s_last_ctx;

static void reset_counters(void)
{
    s_updating_calls = 0;
    s_success_calls  = 0;
    s_restore_calls  = 0;
    s_last_pct       = -1;
    s_last_ctx       = NULL;
}

static void fake_updating(void *ctx, int pct)
{
    s_updating_calls++;
    s_last_pct = pct;
    s_last_ctx = ctx;
}

static void fake_success(void *ctx)
{
    s_success_calls++;
    s_last_ctx = ctx;
}

static void fake_restore(void *ctx)
{
    s_restore_calls++;
    s_last_ctx = ctx;
}

static bb_ota_led_ops_t k_fake = {fake_updating, fake_success, fake_restore};

// 1: START routes to updating; ctx is passed through
void test_ota_led_start_calls_updating(void)
{
    reset_counters();
    int sentinel;
    bb_ota_led_init(&k_fake, &sentinel);
    bb_ota_led_progress(BB_OTA_PHASE_START, 0);
    TEST_ASSERT_EQUAL_INT(1, s_updating_calls);
    TEST_ASSERT_EQUAL_PTR(&sentinel, s_last_ctx);
}

// 2: PROGRESS routes to updating with correct pct
void test_ota_led_progress_calls_updating_with_pct(void)
{
    reset_counters();
    bb_ota_led_init(&k_fake, NULL);
    int before = s_updating_calls;
    bb_ota_led_progress(BB_OTA_PHASE_PROGRESS, 42);
    TEST_ASSERT_EQUAL_INT(before + 1, s_updating_calls);
    TEST_ASSERT_EQUAL_INT(42, s_last_pct);
}

// 3: SUCCESS routes to success only; restore not called
void test_ota_led_success_calls_success(void)
{
    reset_counters();
    bb_ota_led_init(&k_fake, NULL);
    bb_ota_led_progress(BB_OTA_PHASE_SUCCESS, 100);
    TEST_ASSERT_EQUAL_INT(1, s_success_calls);
    TEST_ASSERT_EQUAL_INT(0, s_restore_calls);
}

// 4: FAIL routes to restore — the bug-fix contract
void test_ota_led_fail_calls_restore(void)
{
    reset_counters();
    bb_ota_led_init(&k_fake, NULL);
    bb_ota_led_progress(BB_OTA_PHASE_FAIL, 0);
    TEST_ASSERT_EQUAL_INT(1, s_restore_calls);
}

// 5: unknown phase hits default -> restore (defensive abort contract)
void test_ota_led_unknown_phase_restores(void)
{
    reset_counters();
    bb_ota_led_init(&k_fake, NULL);
    bb_ota_led_progress((bb_ota_phase_t)999, 0);
    TEST_ASSERT_EQUAL_INT(1, s_restore_calls);
}

// 6: no init (ops=NULL) -> no crash, no counter movement
void test_ota_led_no_init_is_safe(void)
{
    reset_counters();
    bb_ota_led_init(NULL, NULL);
    bb_ota_led_progress(BB_OTA_PHASE_FAIL, 0);
    TEST_ASSERT_EQUAL_INT(0, s_restore_calls);
    TEST_ASSERT_EQUAL_INT(0, s_updating_calls);
    TEST_ASSERT_EQUAL_INT(0, s_success_calls);
}

// 7: ops with success=NULL -> no crash on SUCCESS
void test_ota_led_null_op_is_safe(void)
{
    reset_counters();
    static bb_ota_led_ops_t k_no_success = {fake_updating, NULL, fake_restore};
    bb_ota_led_init(&k_no_success, NULL);
    bb_ota_led_progress(BB_OTA_PHASE_SUCCESS, 0);
    // no crash; success_calls stays 0 (the NULL slot was skipped)
    TEST_ASSERT_EQUAL_INT(0, s_success_calls);
}
