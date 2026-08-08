#include "unity.h"
#include "bb_log.h"
#include "bb_log_internal.h"

#include <stdio.h>
#include <string.h>

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

// B1-1443 PR-1: host bb_log_emit()'s NULL-tag normalization branch
// (platform/host/bb_log/bb_log_emit.c) -- mirrors the ESP-IDF side's own
// "!tag -> '?'" normalization (see bb_log_emit's doc comment in bb_log.h).
void test_bb_log_null_tag_normalizes_instead_of_crashing(void) {
    bb_log_e(NULL, "msg %d", 1);
    TEST_PASS();
}

// bb_log_flush — host build has no async writer (bb_log_i/e/w write
// straight through fprintf, see bb_log.h's non-ESP_PLATFORM branch), so this
// degrades to a synchronous fflush(stdout) and always succeeds.
void test_bb_log_flush_returns_ok(void) {
    bb_log_i("TAG", "before flush");
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_log_flush());
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

// B1-1443 PR-1: bb_log_level_console_letter() -- the pure mapping
// bb_log_emit() uses to build a console-shaped line ("<L> (<ts>) <tag>:
// <msg>") without going through ESP_LOGx/LOG_FORMAT. Mirrors
// bb_log_line_parse()'s own accepted level chars (I/W/E/D/V) plus the '?'
// fallback for BB_LOG_LEVEL_NONE/out-of-range.
void test_bb_log_level_console_letter_error(void) {
    TEST_ASSERT_EQUAL_CHAR('E', bb_log_level_console_letter(BB_LOG_LEVEL_ERROR));
}

void test_bb_log_level_console_letter_warn(void) {
    TEST_ASSERT_EQUAL_CHAR('W', bb_log_level_console_letter(BB_LOG_LEVEL_WARN));
}

void test_bb_log_level_console_letter_info(void) {
    TEST_ASSERT_EQUAL_CHAR('I', bb_log_level_console_letter(BB_LOG_LEVEL_INFO));
}

void test_bb_log_level_console_letter_debug(void) {
    TEST_ASSERT_EQUAL_CHAR('D', bb_log_level_console_letter(BB_LOG_LEVEL_DEBUG));
}

void test_bb_log_level_console_letter_verbose(void) {
    TEST_ASSERT_EQUAL_CHAR('V', bb_log_level_console_letter(BB_LOG_LEVEL_VERBOSE));
}

void test_bb_log_level_console_letter_none(void) {
    TEST_ASSERT_EQUAL_CHAR('?', bb_log_level_console_letter(BB_LOG_LEVEL_NONE));
}

void test_bb_log_level_console_letter_out_of_range(void) {
    TEST_ASSERT_EQUAL_CHAR('?', bb_log_level_console_letter((bb_log_level_t)999));
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
    // Fill registry to capacity (CONFIG_BB_LOG_TAG_REGISTRY_MAX = 24 in native env)
    for (int i = 0; i < 24; i++) {
        char tag[32];
        snprintf(tag, sizeof(tag), "tag_%d", i);
        bb_log_tag_register(tag, BB_LOG_LEVEL_INFO);
    }

    // Verify all 24 are present
    const char *tag_out;
    bb_log_level_t level_out;
    for (int i = 0; i < 24; i++) {
        TEST_ASSERT_TRUE(bb_log_tag_at(i, &tag_out, &level_out));
    }

    // 25th should fail (no crash, registry stays at 24)
    TEST_ASSERT_FALSE(bb_log_tag_at(24, &tag_out, &level_out));

    // Try to register a new tag; should be silently dropped
    bb_log_tag_register("tag_overflow", BB_LOG_LEVEL_INFO);
    TEST_ASSERT_FALSE(bb_log_tag_level("tag_overflow", &level_out));
}

// ============================================================================
// bb_log_emit_build_line / bb_log_emit_build_event_msg (B1-1443 PR-1) --
// the pure record-construction core bb_log_emit() (platform/espidf/bb_log/
// bb_log.c) calls for its console-writer/UDP-mirror line and its structured
// bb_log_event forwarder message. Extracted so this shape has real host
// coverage instead of living entirely inside ESP-IDF-only glue.
// ============================================================================

void test_bb_log_emit_build_line_basic(void) {
    char buf[64];
    size_t len = bb_log_emit_build_line(buf, sizeof(buf), 'I', 1234, "wifi", "connected", 9);
    TEST_ASSERT_EQUAL_STRING("I (1234) wifi: connected\n", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), len);
}

void test_bb_log_emit_build_line_null_out(void) {
    TEST_ASSERT_EQUAL_UINT(0, bb_log_emit_build_line(NULL, 64, 'I', 0, "tag", "msg", 3));
}

void test_bb_log_emit_build_line_zero_cap(void) {
    char buf[64];
    TEST_ASSERT_EQUAL_UINT(0, bb_log_emit_build_line(buf, 0, 'I', 0, "tag", "msg", 3));
}

void test_bb_log_emit_build_line_null_tag_normalizes(void) {
    char buf[64];
    size_t len = bb_log_emit_build_line(buf, sizeof(buf), 'E', 0, NULL, "oops", 4);
    TEST_ASSERT_EQUAL_STRING("E (0) ?: oops\n", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), len);
}

void test_bb_log_emit_build_line_null_msg_is_empty(void) {
    char buf[64];
    size_t len = bb_log_emit_build_line(buf, sizeof(buf), 'W', 5, "tag", NULL, 99);
    TEST_ASSERT_EQUAL_STRING("W (5) tag: \n", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), len);
}

void test_bb_log_emit_build_line_truncates_to_out_cap(void) {
    char buf[16];
    size_t len = bb_log_emit_build_line(buf, sizeof(buf), 'I', 1, "verylongtagname", "message", 7);
    TEST_ASSERT_EQUAL_UINT(sizeof(buf) - 1, len);
    TEST_ASSERT_EQUAL_UINT(sizeof(buf) - 1, strlen(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
}

void test_bb_log_emit_build_event_msg_basic(void) {
    bb_log_event_msg_t emsg;
    bb_log_emit_build_event_msg(&emsg, 'I', "wifi", 4242, "connected", 9);
    TEST_ASSERT_TRUE(emsg.structured);
    TEST_ASSERT_EQUAL_CHAR('I', emsg.level);
    TEST_ASSERT_EQUAL_STRING("wifi", emsg.tag);
    TEST_ASSERT_EQUAL_UINT64(4242, emsg.ts_ms);
    TEST_ASSERT_EQUAL_UINT(9, emsg.len);
    TEST_ASSERT_EQUAL_STRING("connected", emsg.line);
}

void test_bb_log_emit_build_event_msg_null_emsg_does_not_crash(void) {
    bb_log_emit_build_event_msg(NULL, 'I', "tag", 0, "msg", 3);
    TEST_PASS();
}

void test_bb_log_emit_build_event_msg_null_tag_normalizes(void) {
    bb_log_event_msg_t emsg;
    bb_log_emit_build_event_msg(&emsg, 'E', NULL, 0, "oops", 4);
    TEST_ASSERT_EQUAL_STRING("?", emsg.tag);
}

void test_bb_log_emit_build_event_msg_null_msg_is_empty(void) {
    bb_log_event_msg_t emsg;
    bb_log_emit_build_event_msg(&emsg, 'W', "tag", 0, NULL, 99);
    TEST_ASSERT_EQUAL_UINT(0, emsg.len);
    TEST_ASSERT_EQUAL_STRING("", emsg.line);
}

void test_bb_log_emit_build_event_msg_tag_truncates(void) {
    bb_log_event_msg_t emsg;
    char long_tag[BB_LOG_EVENT_MSG_TAG_MAX + 32];
    memset(long_tag, 'x', sizeof(long_tag) - 1);
    long_tag[sizeof(long_tag) - 1] = '\0';

    bb_log_emit_build_event_msg(&emsg, 'I', long_tag, 0, "msg", 3);
    TEST_ASSERT_EQUAL_UINT(BB_LOG_EVENT_MSG_TAG_MAX - 1, strlen(emsg.tag));
}

// B1-1443 PR-1 review round 2, finding 1/5: pins bb_log_emit_build_line()'s
// BY-DESIGN plain (no-ANSI-colour) output -- see the doc comment on the
// declaration in bb_log_internal.h. Asserts both that no ESC (0x1b) byte
// ever appears in the line and that the line matches the exact expected
// plain-format shape, so a future accidental reintroduction of
// LOG_COLOR_*/LOG_RESET_COLOR wrapping is caught here.
void test_bb_log_emit_build_line_has_no_ansi_escape_bytes(void) {
    char buf[64];
    size_t len = bb_log_emit_build_line(buf, sizeof(buf), 'E', 777, "wifi", "boom", 4);
    TEST_ASSERT_EQUAL_STRING("E (777) wifi: boom\n", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), len);
    for (size_t i = 0; i < len; i++) {
        TEST_ASSERT_NOT_EQUAL(0x1b, (unsigned char)buf[i]);
    }
}

void test_bb_log_emit_build_event_msg_msg_truncates(void) {
    bb_log_event_msg_t emsg;
    char long_msg[BB_LOG_EVENT_MSG_LINE_MAX + 64];
    memset(long_msg, 'y', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    bb_log_emit_build_event_msg(&emsg, 'I', "tag", 0, long_msg, strlen(long_msg));
    TEST_ASSERT_EQUAL_UINT(BB_LOG_EVENT_MSG_LINE_MAX - 1, emsg.len);
    TEST_ASSERT_EQUAL_UINT(BB_LOG_EVENT_MSG_LINE_MAX - 1, strlen(emsg.line));
}
