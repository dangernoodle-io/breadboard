// B1-893: bb_ntp_last_sync_unix() lost its only production caller
// (bb_ntp_info.c) when that satellite was deleted; a direct test keeps this
// still-public bb_ntp API surface covered.

#include "unity.h"
#include "bb_ntp.h"

void test_bb_ntp_last_sync_unix_returns_zero_on_host(void)
{
    // Host stub has no real NTP client — must return 0 (never synced).
    TEST_ASSERT_EQUAL_INT64(0, bb_ntp_last_sync_unix());
}
