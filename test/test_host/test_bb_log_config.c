#include "unity.h"
#include "bb_log.h"
#include "bb_log_config.h"
#include "bb_log_internal.h"
#include <string.h>

// ---------------------------------------------------------------------------
// bb_log_level_from_name — pure slice-based mapper
// ---------------------------------------------------------------------------

void test_bb_log_level_from_name_none(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_name("none", 4, &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_NONE, level);
}

void test_bb_log_level_from_name_error(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_name("error", 5, &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_ERROR, level);
}

void test_bb_log_level_from_name_warn(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_name("warn", 4, &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level);
}

void test_bb_log_level_from_name_info(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_name("info", 4, &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level);
}

void test_bb_log_level_from_name_debug(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_name("debug", 5, &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
}

void test_bb_log_level_from_name_verbose(void) {
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_name("verbose", 7, &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_VERBOSE, level);
}

void test_bb_log_level_from_name_unknown(void) {
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_level_from_name("bogus", 5, &level));
}

void test_bb_log_level_from_name_empty(void) {
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_level_from_name("", 0, &level));
}

void test_bb_log_level_from_name_null_name(void) {
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_level_from_name(NULL, 4, &level));
}

void test_bb_log_level_from_name_null_out(void) {
    TEST_ASSERT_FALSE(bb_log_level_from_name("info", 4, NULL));
}

void test_bb_log_level_from_name_slice_too_long(void) {
    bb_log_level_t level;
    // "verbose" (7) padded past the 16-byte scratch buffer used internally.
    TEST_ASSERT_FALSE(bb_log_level_from_name("verboseverboseX", 16, &level));
}

void test_bb_log_level_from_name_slice_within_larger_string(void) {
    // Confirms this is a genuine slice API — only [name, name+len) is read,
    // not up to the next NUL.
    const char *s = "warnXXXX";
    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_level_from_name(s, 4, &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level);
}

// ---------------------------------------------------------------------------
// bb_log_config_apply_kv — bb_str_kv_cb_t-shaped per-pair callback
// ---------------------------------------------------------------------------

void test_bb_log_config_apply_kv_valid(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_kv("wifi", 4, "debug", 5, NULL);

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("wifi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
}

void test_bb_log_config_apply_kv_unknown_level_skipped(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_kv("wifi", 4, "bogus", 5, NULL);

    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_tag_level("wifi", &level));
}

void test_bb_log_config_apply_kv_empty_value_skipped(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_kv("wifi", 4, "", 0, NULL);

    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_tag_level("wifi", &level));
}

void test_bb_log_config_apply_kv_tag_truncated_to_scratch_buffer(void) {
    _bb_log_registry_reset();
    // Key longer than the 32-byte internal scratch buffer — must not overflow.
    const char *key = "this_tag_name_is_way_longer_than_the_thirty_two_byte_scratch_buffer";
    bb_log_config_apply_kv(key, strlen(key), "info", 4, NULL);

    // Registered tag must be the key clamped to sizeof(tag_buf)-1 (31) bytes,
    // NUL-terminated — a regression in the clamp math would register either
    // the full untruncated key or a garbage/short tag instead.
    const char *expected = "this_tag_name_is_way_longer_tha";
    TEST_ASSERT_EQUAL_size_t(31, strlen(expected));

    const char *tag_out;
    bb_log_level_t level_out;
    TEST_ASSERT_TRUE(bb_log_tag_at(0, &tag_out, &level_out));
    TEST_ASSERT_EQUAL_STRING(expected, tag_out);
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level_out);
}

void test_bb_log_config_apply_kv_malformed_pair_never_reaches_callback(void) {
    // bb_str_kv_parse itself skips entries with no '=' before ever invoking the
    // callback — verified at the bb_log_config_apply_levels integration
    // level, since bb_log_config_apply_kv can't observe an entry that never
    // reaches it.
    _bb_log_registry_reset();
    bb_log_config_apply_levels("wifi-debug");

    const char *tag_out;
    bb_log_level_t level_out;
    TEST_ASSERT_FALSE(bb_log_tag_at(0, &tag_out, &level_out));
}

// ---------------------------------------------------------------------------
// bb_log_config_apply_levels — full comma-separated string
// ---------------------------------------------------------------------------

void test_bb_log_config_apply_levels_null(void) {
    bb_log_config_apply_levels(NULL);
    TEST_PASS();
}

void test_bb_log_config_apply_levels_empty(void) {
    bb_log_config_apply_levels("");
    TEST_PASS();
}

void test_bb_log_config_apply_levels_single_pair(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_levels("httpd=warn");

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("httpd", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level);
}

void test_bb_log_config_apply_levels_multiple_pairs(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_levels("wifi=debug,httpd=warn,mqtt=error");

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("wifi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
    TEST_ASSERT_TRUE(bb_log_tag_level("httpd", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level);
    TEST_ASSERT_TRUE(bb_log_tag_level("mqtt", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_ERROR, level);
}

void test_bb_log_config_apply_levels_skips_bad_pair_keeps_good_ones(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_levels("wifi=debug,malformed,httpd=bogus,mqtt=info");

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("wifi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
    TEST_ASSERT_FALSE(bb_log_tag_level("malformed", &level));
    TEST_ASSERT_FALSE(bb_log_tag_level("httpd", &level));
    TEST_ASSERT_TRUE(bb_log_tag_level("mqtt", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level);
}

void test_bb_log_config_apply_levels_trailing_comma_skips_empty_token(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_levels("wifi=debug,");

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("wifi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
}

void test_bb_log_config_apply_levels_leading_comma_skips_empty_token(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_levels(",wifi=debug");

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("wifi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
}

void test_bb_log_config_apply_levels_doubled_comma_skips_empty_token(void) {
    _bb_log_registry_reset();
    bb_log_config_apply_levels("wifi=debug,,httpd=warn");

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("wifi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
    TEST_ASSERT_TRUE(bb_log_tag_level("httpd", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_WARN, level);
}

// ---------------------------------------------------------------------------
// bb_log_config_apply — default level + levels string
// ---------------------------------------------------------------------------

void test_bb_log_config_apply_valid_default_sets_global(void) {
    _bb_log_registry_reset();
    bb_err_t rc = bb_log_config_apply("debug", "");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("*", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_DEBUG, level);
}

void test_bb_log_config_apply_invalid_default_is_non_fatal(void) {
    _bb_log_registry_reset();
    bb_err_t rc = bb_log_config_apply("not_a_level", "");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Invalid default must not register "*" at all — platform default stands.
    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_tag_level("*", &level));
}

void test_bb_log_config_apply_null_default_is_non_fatal(void) {
    _bb_log_registry_reset();
    bb_err_t rc = bb_log_config_apply(NULL, "");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_log_level_t level;
    TEST_ASSERT_FALSE(bb_log_tag_level("*", &level));
}

void test_bb_log_config_apply_default_plus_levels(void) {
    _bb_log_registry_reset();
    bb_err_t rc = bb_log_config_apply("info", "wifi=verbose");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("*", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level);
    TEST_ASSERT_TRUE(bb_log_tag_level("wifi", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_VERBOSE, level);
}

// ---------------------------------------------------------------------------
// bb_log_config_init — Kconfig-bound wrapper (default host build flags:
// BB_LOG_DEFAULT_LEVEL_STR="info", BB_LOG_LEVELS_STR="")
// ---------------------------------------------------------------------------

void test_bb_log_config_init_applies_default(void) {
    _bb_log_registry_reset();
    bb_err_t rc = bb_log_config_init();
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_log_level_t level;
    TEST_ASSERT_TRUE(bb_log_tag_level("*", &level));
    TEST_ASSERT_EQUAL(BB_LOG_LEVEL_INFO, level);
}
