// Tests for bb_partition — host mock implementation.
#include "unity.h"
#include "bb_partition.h"
#include <string.h>

// 1: list returns count == 5
void test_bb_partition_list_count(void)
{
    bb_partition_info_t buf[8];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_list(buf, 8, &count));
    TEST_ASSERT_EQUAL_UINT(5, count);
}

// 2: ota_0 has running==true, subtype "ota_0", offset 0x20000
void test_bb_partition_ota0_running(void)
{
    bb_partition_info_t buf[8];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_list(buf, 8, &count));
    // ota_0 is index 2
    TEST_ASSERT_EQUAL_STRING("ota_0", buf[2].subtype);
    TEST_ASSERT_EQUAL_UINT32(0x020000, buf[2].offset);
    TEST_ASSERT_TRUE(buf[2].running);
}

// 3: ota_1 has next_ota==true
void test_bb_partition_ota1_next_ota(void)
{
    bb_partition_info_t buf[8];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_list(buf, 8, &count));
    // ota_1 is index 3
    TEST_ASSERT_TRUE(buf[3].next_ota);
    TEST_ASSERT_EQUAL_STRING("ota_1", buf[3].subtype);
}

// 4: coredump present with subtype "coredump"
void test_bb_partition_coredump_present(void)
{
    bb_partition_info_t buf[8];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_list(buf, 8, &count));
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(buf[i].subtype, "coredump") == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "coredump partition not found");
}

// 5: cap-truncation: cap=2 still sets *count=5, fills 2 entries
void test_bb_partition_list_cap_truncation(void)
{
    bb_partition_info_t buf[2];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_list(buf, 2, &count));
    TEST_ASSERT_EQUAL_UINT(5, count);
    // first two filled: nvs and otadata
    TEST_ASSERT_EQUAL_STRING("nvs",     buf[0].subtype);
    TEST_ASSERT_EQUAL_STRING("otadata", buf[1].subtype);
}

// 6: get_running fills ota_0
void test_bb_partition_get_running(void)
{
    bb_partition_info_t info;
    memset(&info, 0, sizeof(info));
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_get_running(&info));
    TEST_ASSERT_EQUAL_STRING("ota_0", info.subtype);
    TEST_ASSERT_TRUE(info.running);
    TEST_ASSERT_EQUAL_UINT32(0x020000, info.offset);
}

// 7: get_next_ota fills ota_1
void test_bb_partition_get_next_ota(void)
{
    bb_partition_info_t info;
    memset(&info, 0, sizeof(info));
    TEST_ASSERT_EQUAL(BB_OK, bb_partition_get_next_ota(&info));
    TEST_ASSERT_EQUAL_STRING("ota_1", info.subtype);
    TEST_ASSERT_TRUE(info.next_ota);
}
