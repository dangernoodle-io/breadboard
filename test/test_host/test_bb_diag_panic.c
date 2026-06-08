#include "unity.h"
#include "bb_diag.h"
#include "bb_core.h"

void test_bb_diag_panic_available_returns_false_on_host(void)
{
    // Host implementation always returns false
    TEST_ASSERT_FALSE(bb_diag_panic_available());
}

void test_bb_diag_panic_get_returns_not_found_on_host(void)
{
    char buf[256];
    size_t len = sizeof(buf);
    bb_err_t err = bb_diag_panic_get(buf, &len);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

void test_bb_diag_panic_get_invalid_args(void)
{
    char buf[256];
    size_t len = 0;
    // Zero-length buffer is invalid
    bb_err_t err = bb_diag_panic_get(buf, &len);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    // NULL pointer is invalid (on host this won't crash, but still NOT_FOUND)
    len = sizeof(buf);
    err = bb_diag_panic_get(NULL, &len);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    // NULL len_inout is invalid
    err = bb_diag_panic_get(buf, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

void test_bb_diag_panic_clear_is_safe_on_host(void)
{
    // Should be a safe no-op on host
    bb_diag_panic_clear();
    TEST_PASS();
}

void test_bb_diag_panic_clear_after_unavailable(void)
{
    // Even if panic isn't available, clear should be safe
    TEST_ASSERT_FALSE(bb_diag_panic_available());
    bb_diag_panic_clear();
    TEST_ASSERT_FALSE(bb_diag_panic_available());
}

void test_bb_diag_abnormal_reset_count_returns_zero_on_host(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_diag_abnormal_reset_count());
}

void test_bb_diag_abnormal_reset_count_clear_is_safe_on_host(void)
{
    // Should be a safe no-op on host
    bb_diag_abnormal_reset_count_clear();
    TEST_ASSERT_EQUAL_UINT32(0, bb_diag_abnormal_reset_count());
}

void test_bb_diag_panic_app_sha_returns_not_found_on_host(void)
{
    char buf[BB_DIAG_PANIC_APP_SHA256_MAX];
    bb_err_t err = bb_diag_panic_app_sha(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

void test_bb_diag_panic_app_sha_invalid_args(void)
{
    char buf[BB_DIAG_PANIC_APP_SHA256_MAX];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_panic_app_sha(NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_panic_app_sha(buf, 0));
}

void test_bb_diag_panic_coredump_erase_is_safe_on_host(void)
{
    // no-op on host — must not crash
    bb_diag_panic_coredump_erase();
    TEST_PASS();
}

void test_bb_diag_panic_coredump_erase_idempotent_on_host(void)
{
    // calling twice must also be safe
    bb_diag_panic_coredump_erase();
    bb_diag_panic_coredump_erase();
    TEST_PASS();
}

// ---- bb_diag_reset_decision pure-function tests ----

// (a) First boot: stored_fp == 0 → count reset to 0, fp must be stored
void test_bb_diag_reset_decision_first_boot_clean(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0, 0xDEADBEEF, 5, false);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_TRUE(r.store_fp);
}

void test_bb_diag_reset_decision_first_boot_abnormal(void)
{
    // Deploy boot is the clean baseline — NOT counted even if reset reason is abnormal
    bb_diag_reset_result_t r = bb_diag_reset_decision(0, 0xDEADBEEF, 3, true);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_TRUE(r.store_fp);
}

// (b) New firmware (stored_fp != running_fp) + abnormal → counter RESETS to 0, new fp stored
void test_bb_diag_reset_decision_new_firmware_abnormal(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0xAAAAAAAA, 0xBBBBBBBB, 7, true);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_TRUE(r.store_fp);
}

// New firmware, clean reset → also resets to 0 and stores fp
void test_bb_diag_reset_decision_new_firmware_clean(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0x11111111, 0x22222222, 10, false);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_TRUE(r.store_fp);
}

// (c) Same firmware + abnormal → count increments by 1
void test_bb_diag_reset_decision_same_firmware_abnormal(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0xCAFEBABE, 0xCAFEBABE, 4, true);
    TEST_ASSERT_EQUAL_UINT32(5, r.new_count);
    TEST_ASSERT_FALSE(r.store_fp);
}

// (d) Same firmware + clean reset → count unchanged
void test_bb_diag_reset_decision_same_firmware_clean(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0x12345678, 0x12345678, 3, false);
    TEST_ASSERT_EQUAL_UINT32(3, r.new_count);
    TEST_ASSERT_FALSE(r.store_fp);
}

// Same firmware, count starts at 0, clean → stays 0
void test_bb_diag_reset_decision_same_firmware_clean_from_zero(void)
{
    bb_diag_reset_result_t r = bb_diag_reset_decision(0xABCDABCD, 0xABCDABCD, 0, false);
    TEST_ASSERT_EQUAL_UINT32(0, r.new_count);
    TEST_ASSERT_FALSE(r.store_fp);
}

// ---- bb_diag_panic_order_copy unit tests ----

// Not-wrapped: length < buf_size, data starts at index 0
void test_panic_order_copy_not_wrapped(void)
{
    const char src[] = "hello world";   // 11 bytes
    char out[64];
    size_t n = bb_diag_panic_order_copy(src, sizeof(src) - 1,
                                         11, 11,  // write_pos=11, not full
                                         out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(11, n);
    TEST_ASSERT_EQUAL_STRING("hello world", out);
}

// Wrapped: length == buf_size, write_pos in the middle
// buf = "DEFABC" with buf_size=6, write_pos=3, length=6 → ordered: "ABCDEF"
void test_panic_order_copy_wrapped(void)
{
    const char src[6] = {'D','E','F','A','B','C'};
    char out[16];
    size_t n = bb_diag_panic_order_copy(src, 6,
                                         6, 3,  // full, write_pos=3
                                         out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(6, n);
    TEST_ASSERT_EQUAL_STRING("ABCDEF", out);
}

// Full buffer, write_pos at 0 (wrapped exactly at boundary)
void test_panic_order_copy_full_write_pos_zero(void)
{
    const char src[4] = {'A','B','C','D'};
    char out[16];
    size_t n = bb_diag_panic_order_copy(src, 4,
                                         4, 0,  // full, write_pos=0 → not actually wrapped in content
                                         out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_STRING("ABCD", out);
}

// Empty buffer (length == 0)
void test_panic_order_copy_empty(void)
{
    const char src[8] = {0};
    char out[16];
    out[0] = 'X';
    size_t n = bb_diag_panic_order_copy(src, 8,
                                         0, 0,
                                         out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_STRING("", out);
}

// Truncation: out_cap smaller than the data
void test_panic_order_copy_truncation(void)
{
    const char src[] = "ABCDEFGHIJ";  // 10 bytes, not wrapped
    char out[5];  // capacity 5 → max 4 bytes + NUL
    size_t n = bb_diag_panic_order_copy(src, 10,
                                         10, 10,  // not full (length < buf_size=10? no, equal — but write_pos=10 == buf_size is the not-wrapped case)
                                         out, sizeof(out));
    // length(10) == buf_size(10): wrapped case, write_pos=10 which equals buf_size
    // first_chunk = buf_size - write_pos = 0, so second_chunk handles everything
    // to_copy = min(10, 4) = 4
    TEST_ASSERT_EQUAL_size_t(4, n);
    out[4] = '\0';
    TEST_ASSERT_EQUAL_STRING("ABCD", out);
}

// Truncation on wrapped buffer
void test_panic_order_copy_truncation_wrapped(void)
{
    // buf = "56123" with buf_size=5, write_pos=2, length=5 → ordered: "12356"
    const char src[5] = {'5','6','1','2','3'};   // wait: write_pos=2 → oldest is at index 2
    // actual ordered = src[2..4] + src[0..1] = "123" + "56" = "12356"
    char out[4];  // capacity 4 → max 3 bytes + NUL
    size_t n = bb_diag_panic_order_copy(src, 5,
                                         5, 2,
                                         out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_STRING("123", out);
}

// NULL buf returns 0 and NUL-terminates out
void test_panic_order_copy_null_buf(void)
{
    char out[8] = "garbage";
    size_t n = bb_diag_panic_order_copy(NULL, 8, 4, 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
}

// NULL out returns 0
void test_panic_order_copy_null_out(void)
{
    const char src[8] = {0};
    size_t n = bb_diag_panic_order_copy(src, 8, 4, 0, NULL, 8);
    TEST_ASSERT_EQUAL_size_t(0, n);
}

// out_cap of 1 produces empty string (only room for NUL)
void test_panic_order_copy_out_cap_one(void)
{
    const char src[] = "hello";
    char out[1];
    size_t n = bb_diag_panic_order_copy(src, 5, 5, 0, out, 1);
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
}

// ---- bb_diag_scrub_text unit tests ----

void test_bb_diag_scrub_text_null_safe(void)
{
    // must not crash on NULL
    bb_diag_scrub_text(NULL);
    TEST_PASS();
}

void test_bb_diag_scrub_text_empty_string(void)
{
    char s[] = "";
    bb_diag_scrub_text(s);
    TEST_ASSERT_EQUAL_STRING("", s);
}

void test_bb_diag_scrub_text_printable_unchanged(void)
{
    char s[] = "Task watchdog got triggered: main";
    char expected[] = "Task watchdog got triggered: main";
    bb_diag_scrub_text(s);
    TEST_ASSERT_EQUAL_STRING(expected, s);
}

void test_bb_diag_scrub_text_high_bytes_replaced(void)
{
    // embed 0x9e and 0xcb — non-UTF-8 high bytes seen on-device
    // 0x3f is '?' (printable ASCII) — stays unchanged
    char s[] = "did not reset:\n - d\x9e\xcb\x3f";
    bb_diag_scrub_text(s);
    // 0x9e → '?', 0xcb → '?', 0x3f ('?') stays
    TEST_ASSERT_EQUAL_STRING("did not reset:\n - d???", s);
}

void test_bb_diag_scrub_text_tab_newline_cr_preserved(void)
{
    char s[] = "line1\tline2\nline3\rend";
    bb_diag_scrub_text(s);
    TEST_ASSERT_EQUAL_STRING("line1\tline2\nline3\rend", s);
}

void test_bb_diag_scrub_text_control_chars_replaced(void)
{
    // 0x01 (SOH) and 0x1F (US) are non-printable control chars — must be scrubbed
    char s[] = "\x01hello\x1fworld";
    bb_diag_scrub_text(s);
    TEST_ASSERT_EQUAL_STRING("?hello?world", s);
}

void test_bb_diag_scrub_text_del_replaced(void)
{
    // 0x7F (DEL) is not in the printable range 0x20..0x7E
    char s[] = "abc\x7f" "def";
    bb_diag_scrub_text(s);
    TEST_ASSERT_EQUAL_STRING("abc?def", s);
}

void test_bb_diag_scrub_text_all_printable_ascii(void)
{
    // full printable ASCII range 0x20..0x7E unchanged
    char s[96];
    for (int i = 0; i < 95; i++) {
        s[i] = (char)(0x20 + i);
    }
    s[95] = '\0';
    char expected[96];
    for (int i = 0; i < 95; i++) {
        expected[i] = (char)(0x20 + i);
    }
    expected[95] = '\0';
    bb_diag_scrub_text(s);
    TEST_ASSERT_EQUAL_STRING(expected, s);
}
