#include "unity.h"
#include "bb_nv.h"
#include "bb_mdns.h"

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
