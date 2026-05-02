#include "unity.h"
#include "bb_byte_order.h"
#include <string.h>

// Test bb_load_be32 with known constant
void test_bb_load_be32_constant(void)
{
    uint8_t bytes[] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint32_t result = bb_load_be32(bytes);
    TEST_ASSERT_EQUAL_UINT32(0xaabbccdd, result);
}

// Test bb_load_le32 with same bytes
void test_bb_load_le32_constant(void)
{
    uint8_t bytes[] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint32_t result = bb_load_le32(bytes);
    TEST_ASSERT_EQUAL_UINT32(0xddccbbaa, result);
}

// Test bb_store_be32 round-trip
void test_bb_store_be32_round_trip(void)
{
    uint8_t buf[4];
    bb_store_be32(buf, 0xaabbccdd);
    TEST_ASSERT_EQUAL_UINT8(0xaa, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xbb, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xcc, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0xdd, buf[3]);
}

// Test bb_store_le32 round-trip
void test_bb_store_le32_round_trip(void)
{
    uint8_t buf[4];
    bb_store_le32(buf, 0xaabbccdd);
    TEST_ASSERT_EQUAL_UINT8(0xdd, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xcc, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xbb, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0xaa, buf[3]);
}

// Test 32-bit big-endian load/store round-trip property
void test_bb_load_be32_store_be32_round_trip(void)
{
    uint8_t buf[4];
    uint32_t test_values[] = {0x00000000, 0xffffffff, 0x12345678, 0x80000001, 0x00ff00ff};

    for (size_t i = 0; i < sizeof(test_values) / sizeof(test_values[0]); i++) {
        bb_store_be32(buf, test_values[i]);
        uint32_t loaded = bb_load_be32(buf);
        TEST_ASSERT_EQUAL_UINT32(test_values[i], loaded);
    }
}

// Test 32-bit little-endian load/store round-trip property
void test_bb_load_le32_store_le32_round_trip(void)
{
    uint8_t buf[4];
    uint32_t test_values[] = {0x00000000, 0xffffffff, 0x12345678, 0x80000001, 0x00ff00ff};

    for (size_t i = 0; i < sizeof(test_values) / sizeof(test_values[0]); i++) {
        bb_store_le32(buf, test_values[i]);
        uint32_t loaded = bb_load_le32(buf);
        TEST_ASSERT_EQUAL_UINT32(test_values[i], loaded);
    }
}

// Test bb_load_be16 constant
void test_bb_load_be16_constant(void)
{
    uint8_t bytes[] = {0xaa, 0xbb};
    uint16_t result = bb_load_be16(bytes);
    TEST_ASSERT_EQUAL_UINT16(0xaabb, result);
}

// Test bb_load_le16 constant
void test_bb_load_le16_constant(void)
{
    uint8_t bytes[] = {0xaa, 0xbb};
    uint16_t result = bb_load_le16(bytes);
    TEST_ASSERT_EQUAL_UINT16(0xbbaa, result);
}

// Test 16-bit big-endian load/store round-trip
void test_bb_load_be16_store_be16_round_trip(void)
{
    uint8_t buf[2];
    uint16_t test_values[] = {0x0000, 0xffff, 0x1234, 0x8000, 0x00ff};

    for (size_t i = 0; i < sizeof(test_values) / sizeof(test_values[0]); i++) {
        bb_store_be16(buf, test_values[i]);
        uint16_t loaded = bb_load_be16(buf);
        TEST_ASSERT_EQUAL_UINT16(test_values[i], loaded);
    }
}

// Test 16-bit little-endian load/store round-trip
void test_bb_load_le16_store_le16_round_trip(void)
{
    uint8_t buf[2];
    uint16_t test_values[] = {0x0000, 0xffff, 0x1234, 0x8000, 0x00ff};

    for (size_t i = 0; i < sizeof(test_values) / sizeof(test_values[0]); i++) {
        bb_store_le16(buf, test_values[i]);
        uint16_t loaded = bb_load_le16(buf);
        TEST_ASSERT_EQUAL_UINT16(test_values[i], loaded);
    }
}

// Test alignment safety: load from misaligned buffer (offset +1)
void test_bb_load_be32_misaligned(void)
{
    uint8_t buf[] = {0x00, 0xaa, 0xbb, 0xcc, 0xdd};
    // Load from &buf[1], which is intentionally misaligned
    uint32_t result = bb_load_be32(&buf[1]);
    TEST_ASSERT_EQUAL_UINT32(0xaabbccdd, result);
}

// Test alignment safety: store to misaligned buffer
void test_bb_store_be32_misaligned(void)
{
    uint8_t buf[5];
    memset(buf, 0, sizeof(buf));
    // Store to &buf[1], which is intentionally misaligned
    bb_store_be32(&buf[1], 0xaabbccdd);
    TEST_ASSERT_EQUAL_UINT8(0xaa, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xbb, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0xcc, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0xdd, buf[4]);
}

// Test alignment safety for 16-bit load from misaligned buffer
void test_bb_load_be16_misaligned(void)
{
    uint8_t buf[] = {0x00, 0xaa, 0xbb};
    uint16_t result = bb_load_be16(&buf[1]);
    TEST_ASSERT_EQUAL_UINT16(0xaabb, result);
}

// Test alignment safety for 16-bit store to misaligned buffer
void test_bb_store_be16_misaligned(void)
{
    uint8_t buf[3];
    memset(buf, 0, sizeof(buf));
    bb_store_be16(&buf[1], 0xaabb);
    TEST_ASSERT_EQUAL_UINT8(0xaa, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xbb, buf[2]);
}
