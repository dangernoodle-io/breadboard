#include "unity.h"
#include "bb_board.h"

_Static_assert(sizeof(size_t) >= 4, "size_t must be at least 4 bytes");

void test_bb_board_heap_free_total_callable(void)
{
    size_t sz = bb_board_heap_free_total();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_heap_free_internal_callable(void)
{
    size_t sz = bb_board_heap_free_internal();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_heap_minimum_ever_callable(void)
{
    size_t sz = bb_board_heap_minimum_ever();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_heap_largest_free_block_callable(void)
{
    size_t sz = bb_board_heap_largest_free_block();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_chip_revision_callable(void)
{
    uint32_t rev = bb_board_chip_revision();
    TEST_ASSERT_EQUAL_UINT32(rev, rev);  // Sanity: no crash, returns uint32_t
}

void test_bb_board_cpu_freq_mhz_callable(void)
{
    uint32_t freq = bb_board_cpu_freq_mhz();
    TEST_ASSERT_EQUAL_UINT32(freq, freq);  // Sanity: no crash, returns uint32_t
}

void test_bb_board_heap_internal_free_callable(void)
{
    size_t sz = bb_board_heap_internal_free();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_heap_internal_total_callable(void)
{
    size_t sz = bb_board_heap_internal_total();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_psram_free_callable(void)
{
    size_t sz = bb_board_psram_free();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_psram_total_callable(void)
{
    size_t sz = bb_board_psram_total();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_rtc_used_callable(void)
{
    size_t sz = bb_board_rtc_used();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_rtc_total_callable(void)
{
    size_t sz = bb_board_rtc_total();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_heap_internal_minimum_ever_callable(void)
{
    size_t sz = bb_board_heap_internal_minimum_ever();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_dram_static_bytes_callable(void)
{
    size_t sz = bb_board_dram_static_bytes();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_dram_static_bytes_returns_zero_on_host(void)
{
    // Host stub has no linker symbols — must return 0.
    TEST_ASSERT_EQUAL_size_t(0, bb_board_dram_static_bytes());
}

// B1-893: bb_board_get_cores/bb_board_get_chip_model lost their only
// production caller (bb_info_build.c) when bb_info was deleted; direct
// tests keep this still-public bb_board API surface covered.

void test_bb_board_get_cores_callable(void)
{
    uint8_t cores = bb_board_get_cores();
    TEST_ASSERT_EQUAL_UINT8(cores, cores);  // Sanity: no crash, returns uint8_t
}

void test_bb_board_get_chip_model_null_out_returns_invalid_arg(void)
{
    char buf[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_board_get_chip_model(NULL, sizeof(buf)));
}

void test_bb_board_get_chip_model_zero_size_returns_invalid_arg(void)
{
    char buf[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_board_get_chip_model(buf, 0));
}

void test_bb_board_get_chip_model_writes_host_string(void)
{
    char buf[16] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_board_get_chip_model(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("host", buf);
}

// B1-... : bb_pub/bb_sink_* cluster deletion left these still-public bb_board
// primitives with no direct test — they were only ever covered transitively
// through the deleted cluster's tests. Direct tests restore real coverage.

void test_bb_board_get_flash_size_returns_zero_on_host(void)
{
    // Host stub has no real flash — must return 0 (matches get_app_size,
    // get_free_heap: real values require an ESP-IDF partition table).
    TEST_ASSERT_EQUAL_UINT32(0, bb_board_get_flash_size());
}

void test_bb_board_get_app_size_returns_zero_on_host(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_board_get_app_size());
}

void test_bb_board_get_mac_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_board_get_mac(NULL, 16));
}

void test_bb_board_get_mac_zero_size_returns_invalid_arg(void)
{
    char buf[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_board_get_mac(buf, 0));
}

void test_bb_board_get_mac_writes_empty_string_on_host(void)
{
    // Host stub has no real MAC source — must return BB_OK with an empty
    // string (distinct from an error: callers can format "" without special-casing).
    char buf[16] = "not-empty";
    TEST_ASSERT_EQUAL(BB_OK, bb_board_get_mac(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_bb_board_get_reset_reason_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_board_get_reset_reason(NULL, 16));
}

void test_bb_board_get_reset_reason_zero_size_returns_invalid_arg(void)
{
    char buf[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_board_get_reset_reason(buf, 0));
}

void test_bb_board_get_reset_reason_writes_power_on_string(void)
{
    char buf[16] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_board_get_reset_reason(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("power-on", buf);
}

void test_bb_board_heap_internal_largest_free_block_callable(void)
{
    // Delegates to bb_meminfo (SSOT) — host stub zeros every field.
    size_t sz = bb_board_heap_internal_largest_free_block();
    TEST_ASSERT_EQUAL_size_t(0, sz);
}
