#include "unity.h"
#include "bb_storage_nvs.h"

// bb_storage_nvs_flash_init()'s real erase-and-retry body only exists under
// ESP_PLATFORM (nvs_flash_init()/nvs_flash_erase() are device-only), so the
// once-guard contract it relies on (bb_once_run: fn() runs exactly once
// across any number of callers) is exercised here via the host-only
// injection seam (BB_STORAGE_NVS_TESTING, !ESP_PLATFORM) declared in
// bb_storage_nvs.h — bb_storage_nvs_flash_init_call_for_test() drives the
// SAME bb_once_run(&once, fn, NULL) call that on-device flash_init_once()
// sits behind, against a fake counting body.

void test_bb_storage_nvs_flash_init_body_runs_exactly_once_across_repeated_calls(void)
{
    bb_storage_nvs_flash_init_reset_for_test();
    TEST_ASSERT_EQUAL(0, bb_storage_nvs_flash_init_run_count_for_test());

    bb_storage_nvs_flash_init_call_for_test();
    bb_storage_nvs_flash_init_call_for_test();
    bb_storage_nvs_flash_init_call_for_test();

    TEST_ASSERT_EQUAL(1, bb_storage_nvs_flash_init_run_count_for_test());
}

void test_bb_storage_nvs_flash_init_reset_allows_body_to_run_again(void)
{
    bb_storage_nvs_flash_init_reset_for_test();
    bb_storage_nvs_flash_init_call_for_test();
    TEST_ASSERT_EQUAL(1, bb_storage_nvs_flash_init_run_count_for_test());

    bb_storage_nvs_flash_init_reset_for_test();
    TEST_ASSERT_EQUAL(0, bb_storage_nvs_flash_init_run_count_for_test());

    bb_storage_nvs_flash_init_call_for_test();
    TEST_ASSERT_EQUAL(1, bb_storage_nvs_flash_init_run_count_for_test());
}
