#include "unity.h"
#include "bb_mdns_refresh_decision.h"

// B1-539: pure decision logic for whether the periodic browse-refresh may
// recreate a browse subscription after mdns_browse_delete. Three cases:
//   - delete OK -> recreate is safe
//   - delete NOT_FOUND (browse already gone) -> recreate is safe
//   - delete enqueue failed (NO_MEM / queue full) -> skip recreate this cycle

void test_bb_mdns_refresh_should_recreate_ok_returns_true(void)
{
    TEST_ASSERT_TRUE(bb_mdns_refresh_should_recreate(BB_MDNS_REFRESH_DELETE_OK));
}

void test_bb_mdns_refresh_should_recreate_other_returns_true(void)
{
    TEST_ASSERT_TRUE(bb_mdns_refresh_should_recreate(BB_MDNS_REFRESH_DELETE_OTHER));
}

void test_bb_mdns_refresh_should_recreate_no_mem_returns_false(void)
{
    TEST_ASSERT_FALSE(bb_mdns_refresh_should_recreate(BB_MDNS_REFRESH_DELETE_NO_MEM));
}
