// Tests for bb_nv_erase_namespace (B1-290).
//
// The DELETE /api/nvs route handler this file used to also cover moved to
// bb_storage_http (B1-757, see test_bb_storage_http_routes.c) — bb_nv itself
// still owns bb_nv_erase_namespace (the generic-KV forwarder), unrelated to
// that HTTP route's rehoming.

#include "unity.h"
#include "bb_nv.h"

void test_nv_erase_namespace_null_returns_err(void)
{
    bb_err_t err = bb_nv_erase_namespace(NULL);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_nv_erase_namespace_removes_all_keys(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("test_ns", "key_a", "val_a");
    bb_nv_set_str("test_ns", "key_b", "val_b");
    TEST_ASSERT_TRUE(bb_nv_exists("test_ns", "key_a"));
    TEST_ASSERT_TRUE(bb_nv_exists("test_ns", "key_b"));

    bb_err_t err = bb_nv_erase_namespace("test_ns");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    TEST_ASSERT_FALSE(bb_nv_exists("test_ns", "key_a"));
    TEST_ASSERT_FALSE(bb_nv_exists("test_ns", "key_b"));
}

void test_nv_erase_namespace_does_not_affect_other_namespaces(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("ns_a", "key", "v");
    bb_nv_set_str("ns_b", "key", "v");

    bb_nv_erase_namespace("ns_a");

    TEST_ASSERT_FALSE(bb_nv_exists("ns_a", "key"));
    TEST_ASSERT_TRUE(bb_nv_exists("ns_b", "key"));
}

void test_nv_erase_namespace_empty_is_ok(void)
{
    bb_nv_host_str_store_reset();
    bb_err_t err = bb_nv_erase_namespace("nonexistent_ns");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

void test_nv_erase_namespace_then_set_works(void)
{
    bb_nv_host_str_store_reset();
    bb_nv_set_str("my_ns", "k", "old");
    bb_nv_erase_namespace("my_ns");
    bb_err_t err = bb_nv_set_str("my_ns", "k", "new");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_TRUE(bb_nv_exists("my_ns", "k"));
}
