#include "unity.h"
#include "bb_mem.h"
#include <string.h>

// bb_malloc_prefer_spiram returns a usable (writable) block.
void test_bb_mem_malloc_returns_usable_block(void)
{
    unsigned char *p = bb_malloc_prefer_spiram(64);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0xAB, 64);
    TEST_ASSERT_EQUAL_UINT8(0xAB, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, p[63]);
    bb_mem_free(p);
}

// bb_calloc_prefer_spiram zero-initialises the whole block.
void test_bb_mem_calloc_zeroes(void)
{
    unsigned char *p = bb_calloc_prefer_spiram(16, 4); // 64 bytes
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, p[i]);
    }
    bb_mem_free(p);
}

// bb_mem_free(NULL) is a no-op (must not crash).
void test_bb_mem_free_null_is_safe(void)
{
    bb_mem_free(NULL);
    TEST_PASS();
}
