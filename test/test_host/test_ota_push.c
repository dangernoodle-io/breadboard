#include "unity.h"
#include "bb_ota_push.h"

// Content-length validation tests
#define TEST_MAX_SIZE (4 * 1024 * 1024)  // 4 MB — matches Kconfig default

void test_ota_push_validate_content_len_zero_returns_400(void)
{
    TEST_ASSERT_EQUAL_INT(400, bb_ota_push_validate_content_len(0, TEST_MAX_SIZE));
}

void test_ota_push_validate_content_len_negative_returns_400(void)
{
    TEST_ASSERT_EQUAL_INT(400, bb_ota_push_validate_content_len(-1, TEST_MAX_SIZE));
}

void test_ota_push_validate_content_len_oversized_returns_413(void)
{
    TEST_ASSERT_EQUAL_INT(413, bb_ota_push_validate_content_len(TEST_MAX_SIZE + 1, TEST_MAX_SIZE));
}

void test_ota_push_validate_content_len_at_max_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_ota_push_validate_content_len(TEST_MAX_SIZE, TEST_MAX_SIZE));
}

void test_ota_push_validate_content_len_valid_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_ota_push_validate_content_len(65536, TEST_MAX_SIZE));
}

// Deadline ms tests
#define TEST_FLOOR_MS  30000u
#define TEST_CEIL_MS   240000u
#define TEST_MIN_BPS   10240

void test_ota_push_deadline_ms_small_content_returns_floor(void)
{
    // 1024 * 1000 / 10240 = 100ms < floor → floor
    TEST_ASSERT_EQUAL_UINT32(TEST_FLOOR_MS,
        bb_ota_push_deadline_ms(1024, TEST_MIN_BPS, TEST_FLOOR_MS, TEST_CEIL_MS));
}

void test_ota_push_deadline_ms_mid_content_within_bounds(void)
{
    // 2_048_000 * 1000 / 10240 = 200000ms; within [30000, 240000]
    TEST_ASSERT_EQUAL_UINT32(200000u,
        bb_ota_push_deadline_ms(2048000, TEST_MIN_BPS, TEST_FLOOR_MS, 240000u));
}

void test_ota_push_deadline_ms_large_content_clamped_to_ceil(void)
{
    // 10_000_000 * 1000 / 10240 = 976562ms > ceil → ceil
    TEST_ASSERT_EQUAL_UINT32(TEST_CEIL_MS,
        bb_ota_push_deadline_ms(10000000, TEST_MIN_BPS, TEST_FLOOR_MS, TEST_CEIL_MS));
}

void test_ota_push_deadline_ms_zero_content_len_returns_floor(void)
{
    TEST_ASSERT_EQUAL_UINT32(TEST_FLOOR_MS,
        bb_ota_push_deadline_ms(0, TEST_MIN_BPS, TEST_FLOOR_MS, TEST_CEIL_MS));
}

void test_ota_push_deadline_ms_negative_content_len_returns_floor(void)
{
    TEST_ASSERT_EQUAL_UINT32(TEST_FLOOR_MS,
        bb_ota_push_deadline_ms(-1, TEST_MIN_BPS, TEST_FLOOR_MS, TEST_CEIL_MS));
}

void test_ota_push_deadline_ms_zero_min_bps_returns_floor(void)
{
    TEST_ASSERT_EQUAL_UINT32(TEST_FLOOR_MS,
        bb_ota_push_deadline_ms(1335312, 0, TEST_FLOOR_MS, TEST_CEIL_MS));
}

void test_ota_push_deadline_ms_realistic_case(void)
{
    // 1_335_312 * 1000 / 10240 = 130401ms; within [30000, 240000]
    TEST_ASSERT_EQUAL_UINT32(130401u,
        bb_ota_push_deadline_ms(1335312, TEST_MIN_BPS, TEST_FLOOR_MS, TEST_CEIL_MS));
}


