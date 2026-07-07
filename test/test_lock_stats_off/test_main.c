#include "unity.h"
#include "bb_lock.h"

// Dedicated PlatformIO test env (native_lock_stats_off, see platformio.ini)
// that builds WITHOUT -DBB_LOCK_STATS_ENABLE — i.e. at the Kconfig default
// (0). This is the compile-time counterpart to test_host/test_bb_lock.c's
// runtime-disabled coverage: that suite is built with
// BB_LOCK_STATS_ENABLE=1 and only ever exercises the runtime-off code path
// (bb_lock_stats_set_enabled(false)); it never proves the *compiled-out*
// BB_LOCK_STATS_ENABLE=0 branch — the shipped default — actually compiles
// and behaves correctly. This suite closes that gap with a minimal smoke
// test.

void setUp(void) {}
void tearDown(void) {}

void test_lock_stats_off_get_stats_is_all_zero(void)
{
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
        TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));
    }

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.acquisition_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.contention_count);
    TEST_ASSERT_EQUAL_UINT64(0, s.wait_time_total_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.wait_time_max_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_total_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_max_us);

    // Runtime toggle is a documented no-op at this compile gate.
    bb_lock_stats_set_enabled(true);
    TEST_ASSERT_FALSE(bb_lock_stats_enabled());

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_destroy(&lock));
}

void test_lock_stats_off_lock_unlock_trylock_still_work(void)
{
    bb_lock_config_t cfg = { .name = "off", .category = "smoke" };
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(&cfg, &lock));

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_lock_trylock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_trylock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_destroy(&lock));
    // Double-destroy still detected even with stats compiled out.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_lock_destroy(&lock));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lock_stats_off_get_stats_is_all_zero);
    RUN_TEST(test_lock_stats_off_lock_unlock_trylock_still_work);
    return UNITY_END();
}
