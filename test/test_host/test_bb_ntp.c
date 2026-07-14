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

// B1-...: bb_ntp_is_synced() lost its only direct test when the bb_pub/
// bb_sink_* cluster was deleted (it was only ever covered transitively).
// Host has no real NTP client — always reports not-synced.
void test_bb_ntp_is_synced_returns_false_on_host(void)
{
    TEST_ASSERT_FALSE(bb_ntp_is_synced());
}
