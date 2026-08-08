// test_bb_log_event_line_wire_build -- B1-1443 PR-2 review round 2, finding
// 2: pins bb_log_event_line_wire_build_foreign()'s (components/bb_log_event/
// src/bb_log_event_line_wire_build.c) exact opaque-wrap output, so a future
// change can't quietly reintroduce a re-parse, an ANSI strip, a non-empty
// tag, or a wrong truncation cap without a test failing. This is the actual
// behavior B1-1443 PR-2 introduces (s_forwarder_task's foreign-line branch,
// platform/espidf/bb_log_event/bb_log_event.c) -- previously only proven
// indirectly (and insufficiently) by test_bb_log_event_line_wire_meta_golden.c,
// which only pins the wire SCHEMA, never the forwarder's actual output.

#include "unity.h"

#include "../../components/bb_log_event/bb_log_event_line_wire_priv.h"

#include <string.h>

// A representative foreign console line, ANSI-colored, as ESP-IDF's own
// LOG_FORMAT() under CONFIG_LOG_COLORS would produce it.
void test_bb_log_event_line_wire_build_foreign_wraps_opaque(void)
{
    static const char *const kLine = "\033[0;32mI (1234) wifi: connected\033[0m\n";

    bb_log_event_line_wire_t snap;
    bb_log_event_line_wire_build_foreign(&snap, 5555, kLine, strlen(kLine));

    TEST_ASSERT_EQUAL_INT64(5555, snap.ts);
    TEST_ASSERT_EQUAL_STRING("?", snap.level);
    TEST_ASSERT_EQUAL_STRING("", snap.tag);
    // Verbatim -- no ANSI stripping, no CRLF trimming, no field extraction.
    TEST_ASSERT_EQUAL_STRING(kLine, snap.msg);
}

void test_bb_log_event_line_wire_build_foreign_truncates_safely(void)
{
    char long_line[512];
    memset(long_line, 'x', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = '\0';

    bb_log_event_line_wire_t snap;
    bb_log_event_line_wire_build_foreign(&snap, 1, long_line, strlen(long_line));

    // Truncated to msg's cap, always NUL-terminated, never overruns.
    TEST_ASSERT_EQUAL_UINT(sizeof(snap.msg) - 1, strlen(snap.msg));
    TEST_ASSERT_EQUAL_CHAR('\0', snap.msg[sizeof(snap.msg) - 1]);
    for (size_t i = 0; i < sizeof(snap.msg) - 1; i++) {
        TEST_ASSERT_EQUAL_CHAR('x', snap.msg[i]);
    }
}

void test_bb_log_event_line_wire_build_foreign_null_line_is_empty_msg(void)
{
    bb_log_event_line_wire_t snap;
    bb_log_event_line_wire_build_foreign(&snap, 42, NULL, 99);

    TEST_ASSERT_EQUAL_INT64(42, snap.ts);
    TEST_ASSERT_EQUAL_STRING("?", snap.level);
    TEST_ASSERT_EQUAL_STRING("", snap.tag);
    TEST_ASSERT_EQUAL_STRING("", snap.msg);
}

void test_bb_log_event_line_wire_build_foreign_zero_length_is_empty_msg(void)
{
    bb_log_event_line_wire_t snap;
    bb_log_event_line_wire_build_foreign(&snap, 42, "not empty", 0);

    TEST_ASSERT_EQUAL_STRING("", snap.msg);
}

void test_bb_log_event_line_wire_build_foreign_null_snap_does_not_crash(void)
{
    bb_log_event_line_wire_build_foreign(NULL, 1, "line", 4);
    // No assertion beyond "did not crash" -- a no-op is the contract.
}

// line_len smaller than strlen(line) governs truncation, not the string's
// own NUL terminator -- proves the defensive strnlen clamp is real, not
// decorative.
void test_bb_log_event_line_wire_build_foreign_respects_line_len_shorter_than_strlen(void)
{
    static const char *const kLine = "hello world";

    bb_log_event_line_wire_t snap;
    bb_log_event_line_wire_build_foreign(&snap, 1, kLine, 5);

    TEST_ASSERT_EQUAL_STRING("hello", snap.msg);
}

void test_bb_log_event_line_wire_build_foreign_overwrites_stale_snap_contents(void)
{
    bb_log_event_line_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));

    bb_log_event_line_wire_build_foreign(&snap, 7, "fresh", 5);

    TEST_ASSERT_EQUAL_INT64(7, snap.ts);
    TEST_ASSERT_EQUAL_STRING("?", snap.level);
    TEST_ASSERT_EQUAL_STRING("", snap.tag);
    TEST_ASSERT_EQUAL_STRING("fresh", snap.msg);
}
