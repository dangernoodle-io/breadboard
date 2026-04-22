#include "unity.h"
#include "bb_log.h"

void test_bb_log_error(void) {
    bb_log_e("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_warning(void) {
    bb_log_w("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_info(void) {
    bb_log_i("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_debug(void) {
    bb_log_d("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_verbose(void) {
    bb_log_v("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_zero_args(void) {
    bb_log_e("TAG", "literal");
    TEST_PASS();
}

void test_bb_log_level_from_str_error(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_str("error", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_ERROR, level);
}

void test_bb_log_level_from_str_warn(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_str("warn", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level);
}

void test_bb_log_level_from_str_info(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_str("info", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level);
}

void test_bb_log_level_from_str_debug(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_str("debug", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
}

void test_bb_log_level_from_str_verbose(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_str("verbose", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_VERBOSE, level);
}

void test_bb_log_level_from_str_none(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_str("none", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_NONE, level);
}

void test_bb_log_level_from_str_case_insensitive(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_str("ERROR", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_ERROR, level);
    TEST_ASSERT_TRUE(bb_log_level_from_str("WaRn", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level);
}

void test_bb_log_level_from_str_invalid(void) {
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_level_from_str("invalid", &level));
    TEST_ASSERT_FALSE(bb_log_level_from_str("", &level));
}

void test_bb_log_level_from_str_null(void) {
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_level_from_str(NULL, &level));
    TEST_ASSERT_FALSE(bb_log_level_from_str("info", NULL));
}

void test_bb_log_level_set_noop(void) {
    // Host backend is no-op; just verify it doesn't crash
    bb_log_level_set("test-tag", BB_LOG_LEVEL_DEBUG);
    bb_log_level_set("*", BB_LOG_LEVEL_INFO);
    TEST_PASS();
}

// ============================================================================
// Registry tests
// ============================================================================

void test_bb_log_tag_register_and_level(void) {
    bb_log_tag_register("wifi", BB_LOG_LEVEL_INFO);

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("wifi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level);
}

void test_bb_log_tag_register_idempotent(void) {
    bb_log_tag_register("phy", BB_LOG_LEVEL_WARN);

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("phy", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level);

    // Second registration should be no-op
    bb_log_tag_register("phy", BB_LOG_LEVEL_DEBUG);
    TEST_ASSERT_TRUE(bb_log_tag_level("phy", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level); // Level unchanged
}

void test_bb_log_level_set_registers_tag(void) {
    bb_log_level_set("spi", BB_LOG_LEVEL_DEBUG);

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("spi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
}

void test_bb_log_level_set_updates_existing_tag(void) {
    bb_log_level_set("uart", BB_LOG_LEVEL_INFO);

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("uart", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level);

    // Update to different level
    bb_log_level_set("uart", BB_LOG_LEVEL_ERROR);
    TEST_ASSERT_TRUE(bb_log_tag_level("uart", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_ERROR, level);
}

void test_bb_log_tag_level_not_found(void) {
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_tag_level("nonexistent", &level));
}

void test_bb_log_tag_at_iteration(void) {
    bb_log_tag_register("tag1", BB_LOG_LEVEL_ERROR);
    bb_log_tag_register("tag2", BB_LOG_LEVEL_WARN);
    bb_log_tag_register("tag3", BB_LOG_LEVEL_INFO);

    const char *tag_out;
    bb_log_level_t level_out;

    // Iterate through all three
    TEST_ASSERT_TRUE(bb_log_tag_at(0, &tag_out, &level_out));
    TEST_ASSERT_EQUAL_STRING("tag1", tag_out);
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_ERROR, level_out);

    TEST_ASSERT_TRUE(bb_log_tag_at(1, &tag_out, &level_out));
    TEST_ASSERT_EQUAL_STRING("tag2", tag_out);
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level_out);

    TEST_ASSERT_TRUE(bb_log_tag_at(2, &tag_out, &level_out));
    TEST_ASSERT_EQUAL_STRING("tag3", tag_out);
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level_out);

    // Past end
    TEST_ASSERT_FALSE(bb_log_tag_at(3, &tag_out, &level_out));
}

void test_bb_log_level_to_str_none(void) {
    TEST_ASSERT_EQUAL_STRING("none", bb_log_level_to_str(BB_LOG_LEVEL_NONE));
}

void test_bb_log_level_to_str_error(void) {
    TEST_ASSERT_EQUAL_STRING("error", bb_log_level_to_str(BB_LOG_LEVEL_ERROR));
}

void test_bb_log_level_to_str_warn(void) {
    TEST_ASSERT_EQUAL_STRING("warn", bb_log_level_to_str(BB_LOG_LEVEL_WARN));
}

void test_bb_log_level_to_str_info(void) {
    TEST_ASSERT_EQUAL_STRING("info", bb_log_level_to_str(BB_LOG_LEVEL_INFO));
}

void test_bb_log_level_to_str_debug(void) {
    TEST_ASSERT_EQUAL_STRING("debug", bb_log_level_to_str(BB_LOG_LEVEL_DEBUG));
}

void test_bb_log_level_to_str_verbose(void) {
    TEST_ASSERT_EQUAL_STRING("verbose", bb_log_level_to_str(BB_LOG_LEVEL_VERBOSE));
}

void test_bb_log_level_to_str_unknown(void) {
    TEST_ASSERT_EQUAL_STRING("unknown", bb_log_level_to_str((bb_log_level_t)999));
}

void test_bb_log_level_from_str_long_input(void) {
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_level_from_str("this_is_a_very_long_level_name", &level));
}

void test_bb_log_level_set_null_tag(void) {
    bb_log_level_set(NULL, BB_LOG_LEVEL_INFO);
    TEST_PASS();
}

void test_bb_log_tag_register_null_tag(void) {
    bb_log_tag_register(NULL, BB_LOG_LEVEL_INFO);
    TEST_PASS();
}

void test_bb_log_tag_level_null_args(void) {
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_tag_level(NULL, &level));
    TEST_ASSERT_FALSE(bb_log_tag_level("tag", NULL));
}

void test_bb_log_tag_at_null_args(void) {
    const char *tag;
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_tag_at(0, NULL, &level));
    TEST_ASSERT_FALSE(bb_log_tag_at(0, &tag, NULL));
}

void test_bb_log_registry_full(void) {
    // Fill registry to capacity (32 entries)
    for (int i = 0; i < 32; i++) {
        char tag[32];
        snprintf(tag, sizeof(tag), "tag_%d", i);
        bb_log_tag_register(tag, BB_LOG_LEVEL_INFO);
    }

    // Verify all 32 are present
    const char *tag_out;
    bb_log_level_t level_out;
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_TRUE(bb_log_tag_at(i, &tag_out, &level_out));
    }

    // 33rd should fail (no crash, registry stays at 32)
    TEST_ASSERT_FALSE(bb_log_tag_at(32, &tag_out, &level_out));

    // Try to register a new tag; should be silently dropped
    bb_log_tag_register("tag_overflow", BB_LOG_LEVEL_INFO);
    TEST_ASSERT_FALSE(bb_log_tag_level("tag_overflow", &level_out));
}
