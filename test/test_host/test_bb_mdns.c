#include "unity.h"
#include "bb_nv.h"
#include "bb_mdns.h"
#include "bb_mdns_host_test_hooks.h"

void test_bb_mdns_browse_start_null_service(void)
{
    bb_err_t err = bb_mdns_browse_start(NULL, "_tcp", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_browse_start_null_proto(void)
{
    bb_err_t err = bb_mdns_browse_start("_taipanminer", NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_browse_stop_unstarted(void)
{
    bb_err_t err = bb_mdns_browse_stop("_taipanminer", "_tcp");
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_mdns_browse_start_valid(void)
{
    bb_err_t err = bb_mdns_browse_start("_taipanminer", "_tcp", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_mdns_browse_stop_valid(void)
{
    bb_mdns_browse_start("_test", "_tcp", NULL, NULL, NULL);
    bb_err_t err = bb_mdns_browse_stop("_test", "_tcp");
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_mdns_browse_stop_null_service(void)
{
    bb_err_t err = bb_mdns_browse_stop(NULL, "_tcp");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_browse_stop_null_proto(void)
{
    bb_err_t err = bb_mdns_browse_stop("_taipanminer", NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

/* bb_mdns_announce host stub tests.
 * Timer-based auto-coalesce (bb_mdns_set_txt arming esp_timer) is ESP-IDF
 * hardware behaviour — verified by flashing, not here. */

void test_mdns_announce_explicit_increments_counter(void)
{
    bb_mdns_host_reset();
    bb_mdns_announce();
    TEST_ASSERT_EQUAL(1, bb_mdns_host_announce_count());
    bb_mdns_announce();
    TEST_ASSERT_EQUAL(2, bb_mdns_host_announce_count());
}

void test_mdns_set_txt_does_not_announce_immediately(void)
{
    /* On host, set_txt increments the txt counter but NOT the announce counter —
     * coalescing is ESP-only timer behaviour; the announce counter only moves
     * on explicit bb_mdns_announce() calls. */
    bb_mdns_host_reset();
    bb_mdns_set_txt("version", "v0.14.1");
    bb_mdns_set_txt("state", "mining");
    TEST_ASSERT_EQUAL(2, bb_mdns_host_set_txt_count());
    TEST_ASSERT_EQUAL(0, bb_mdns_host_announce_count());
}

void test_mdns_set_txt_null_key_is_safe(void)
{
    bb_mdns_host_reset();
    bb_mdns_set_txt(NULL, "value");
    TEST_ASSERT_EQUAL(0, bb_mdns_host_set_txt_count());
}

void test_mdns_set_txt_null_value_is_safe(void)
{
    bb_mdns_host_reset();
    bb_mdns_set_txt("key", NULL);
    TEST_ASSERT_EQUAL(0, bb_mdns_host_set_txt_count());
}

void test_mdns_host_reset_clears_counters(void)
{
    bb_mdns_announce();
    bb_mdns_set_txt("k", "v");
    bb_mdns_host_reset();
    TEST_ASSERT_EQUAL(0, bb_mdns_host_announce_count());
    TEST_ASSERT_EQUAL(0, bb_mdns_host_set_txt_count());
}
