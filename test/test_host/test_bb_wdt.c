#include "unity.h"
#include "bb_wdt.h"
#include "bb_wdt_test.h"
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

typedef struct {
    int      calls_until_success; /* -1 = never succeeds */
    int      call_count;
    uint32_t last_ms;
} fake_wait_ctx_t;

static bool fake_try_wait(void *ctx, uint32_t ms)
{
    fake_wait_ctx_t *c = (fake_wait_ctx_t *)ctx;
    c->last_ms = ms;
    c->call_count++;
    if (c->calls_until_success < 0) return false;
    if (c->call_count >= c->calls_until_success) return true;
    return false;
}

/* -------------------------------------------------------------------------
 * bb_wdt_park_wait: unsubscribe → wait → resubscribe → feed
 * ---------------------------------------------------------------------- */

void test_bb_wdt_park_wait_resume_unsubscribes_and_resubscribes(void)
{
    bb_wdt_test_reset();
    /* resumes on the single wait */
    fake_wait_ctx_t ctx = { .calls_until_success = 1, .call_count = 0 };
    bool result = bb_wdt_park_wait(fake_try_wait, &ctx, 5000, 1000);
    TEST_ASSERT_TRUE(result);
    /* try_wait called exactly once with the full budget (no slicing) */
    TEST_ASSERT_EQUAL_INT(1, ctx.call_count);
    TEST_ASSERT_EQUAL_UINT32(5000, ctx.last_ms);
    /* removed from the WDT for the park, re-added and fed on resume */
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_feed_count());
}

void test_bb_wdt_park_wait_timeout_still_resubscribes(void)
{
    bb_wdt_test_reset();
    /* never resumes → timeout */
    fake_wait_ctx_t ctx = { .calls_until_success = -1, .call_count = 0 };
    bool result = bb_wdt_park_wait(fake_try_wait, &ctx, 3000, 1000);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL_INT(1, ctx.call_count);
    TEST_ASSERT_EQUAL_UINT32(3000, ctx.last_ms);
    /* must re-add to the WDT even on timeout, else the task is never watched again */
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_feed_count());
}

void test_bb_wdt_park_wait_null_try_wait_returns_false(void)
{
    bb_wdt_test_reset();
    bool result = bb_wdt_park_wait(NULL, NULL, 1000, 100);
    TEST_ASSERT_FALSE(result);
    /* returns before touching the WDT */
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_test_unsubscribe_count());
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_test_subscribe_count());
    TEST_ASSERT_EQUAL_INT(0, bb_wdt_test_feed_count());
}

/* -------------------------------------------------------------------------
 * Host no-op backend: feed/subscribe/unsubscribe counters
 * ---------------------------------------------------------------------- */

void test_bb_wdt_feed_increments_counter(void)
{
    bb_wdt_test_reset();
    bb_wdt_task_feed();
    bb_wdt_task_feed();
    TEST_ASSERT_EQUAL_INT(2, bb_wdt_test_feed_count());
}

void test_bb_wdt_subscribe_increments_counter(void)
{
    bb_wdt_test_reset();
    bb_err_t rc = bb_wdt_task_subscribe();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_subscribe_count());
}

void test_bb_wdt_unsubscribe_increments_counter(void)
{
    bb_wdt_test_reset();
    bb_err_t rc = bb_wdt_task_unsubscribe();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_wdt_test_unsubscribe_count());
}

void test_bb_wdt_set_timeout_noop_on_host(void)
{
    /* just verifies it doesn't crash */
    bb_wdt_set_timeout(60);
    bb_wdt_extend_begin(300);
    bb_wdt_extend_end();
    TEST_PASS();
}
