#include "bb_wdt.h"

#ifdef BB_WDT_TESTING
#include "bb_wdt_test.h"

static int s_feed_count      = 0;
static int s_subscribe_count = 0;
static int s_unsub_count     = 0;

void bb_wdt_test_reset(void)
{
    s_feed_count      = 0;
    s_subscribe_count = 0;
    s_unsub_count     = 0;
}

int bb_wdt_test_feed_count(void)      { return s_feed_count; }
int bb_wdt_test_subscribe_count(void) { return s_subscribe_count; }
int bb_wdt_test_unsubscribe_count(void) { return s_unsub_count; }
#endif /* BB_WDT_TESTING */

void bb_wdt_set_timeout(uint32_t timeout_s)
{
    (void)timeout_s;
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
#ifdef BB_WDT_TESTING
    s_subscribe_count++;
#endif
    return BB_OK;
}

bb_err_t bb_wdt_task_unsubscribe(void)
{
#ifdef BB_WDT_TESTING
    s_unsub_count++;
#endif
    return BB_OK;
}

void bb_wdt_task_feed(void)
{
#ifdef BB_WDT_TESTING
    s_feed_count++;
#endif
}
