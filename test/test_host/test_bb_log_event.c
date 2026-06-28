#include "unity.h"
#include "../../platform/host/bb_log/bb_log_event_parse.h"
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static char s_level;
static char s_tag[64];
static char s_msg[256];

static void parse(const char *line)
{
    s_level = '?';
    s_tag[0] = '\0';
    s_msg[0] = '\0';
    bb_log_event_parse(line, strlen(line), &s_level, s_tag, sizeof(s_tag),
                       s_msg, sizeof(s_msg));
}

// ---------------------------------------------------------------------------
// Parser: well-formed lines
// ---------------------------------------------------------------------------

void test_bb_log_event_parse_info(void)
{
    parse("I (1234) my_tag: hello world");
    TEST_ASSERT_EQUAL_CHAR('I', s_level);
    TEST_ASSERT_EQUAL_STRING("my_tag", s_tag);
    TEST_ASSERT_EQUAL_STRING("hello world", s_msg);
}

void test_bb_log_event_parse_warning(void)
{
    parse("W (0) wifi: reconnecting");
    TEST_ASSERT_EQUAL_CHAR('W', s_level);
    TEST_ASSERT_EQUAL_STRING("wifi", s_tag);
    TEST_ASSERT_EQUAL_STRING("reconnecting", s_msg);
}

void test_bb_log_event_parse_error(void)
{
    parse("E (9999) bb_mqtt: connect failed");
    TEST_ASSERT_EQUAL_CHAR('E', s_level);
    TEST_ASSERT_EQUAL_STRING("bb_mqtt", s_tag);
    TEST_ASSERT_EQUAL_STRING("connect failed", s_msg);
}

void test_bb_log_event_parse_debug(void)
{
    parse("D (100) dbg: value=42");
    TEST_ASSERT_EQUAL_CHAR('D', s_level);
    TEST_ASSERT_EQUAL_STRING("dbg", s_tag);
    TEST_ASSERT_EQUAL_STRING("value=42", s_msg);
}

void test_bb_log_event_parse_verbose(void)
{
    parse("V (200) verbose_tag: all good");
    TEST_ASSERT_EQUAL_CHAR('V', s_level);
    TEST_ASSERT_EQUAL_STRING("verbose_tag", s_tag);
    TEST_ASSERT_EQUAL_STRING("all good", s_msg);
}

// ---------------------------------------------------------------------------
// Parser: ANSI-color-prefixed line
// ---------------------------------------------------------------------------

void test_bb_log_event_parse_ansi_prefix(void)
{
    // ESP-IDF emits e.g. \033[0;32m before the log level char
    parse("\033[0;32mI (500) net: up\033[0m");
    TEST_ASSERT_EQUAL_CHAR('I', s_level);
    TEST_ASSERT_EQUAL_STRING("net", s_tag);
    // msg may contain trailing reset sequence — that is acceptable
    TEST_ASSERT_EQUAL_STRING("up\033[0m", s_msg);
}

// ---------------------------------------------------------------------------
// Parser: malformed → fallback
// ---------------------------------------------------------------------------

void test_bb_log_event_parse_malformed_level(void)
{
    // 'X' is not a valid level char
    parse("X (100) tag: msg");
    TEST_ASSERT_EQUAL_CHAR('?', s_level);
    TEST_ASSERT_EQUAL_STRING("", s_tag);
    // whole trimmed line in msg
    TEST_ASSERT_EQUAL_STRING("X (100) tag: msg", s_msg);
}

void test_bb_log_event_parse_no_paren(void)
{
    parse("I 100 tag: msg");
    TEST_ASSERT_EQUAL_CHAR('?', s_level);
    TEST_ASSERT_EQUAL_STRING("", s_tag);
}

void test_bb_log_event_parse_no_colon_space(void)
{
    // no ": " separator
    parse("I (100) tagmsg");
    TEST_ASSERT_EQUAL_CHAR('?', s_level);
    TEST_ASSERT_EQUAL_STRING("", s_tag);
}

void test_bb_log_event_parse_empty_line(void)
{
    bb_log_event_parse("", 0, &s_level, s_tag, sizeof(s_tag), s_msg, sizeof(s_msg));
    TEST_ASSERT_EQUAL_CHAR('?', s_level);
    TEST_ASSERT_EQUAL_STRING("", s_tag);
    TEST_ASSERT_EQUAL_STRING("", s_msg);
}

void test_bb_log_event_parse_null_line(void)
{
    s_level = 'X';
    bb_log_event_parse(NULL, 5, &s_level, s_tag, sizeof(s_tag), s_msg, sizeof(s_msg));
    TEST_ASSERT_EQUAL_CHAR('?', s_level);
}

// ---------------------------------------------------------------------------
// Parser: trailing CR/LF stripping
// ---------------------------------------------------------------------------

void test_bb_log_event_parse_strips_newline(void)
{
    parse("I (1) tag: msg\n");
    TEST_ASSERT_EQUAL_CHAR('I', s_level);
    TEST_ASSERT_EQUAL_STRING("msg", s_msg);
}

void test_bb_log_event_parse_strips_crlf(void)
{
    parse("W (1) t: m\r\n");
    TEST_ASSERT_EQUAL_CHAR('W', s_level);
    TEST_ASSERT_EQUAL_STRING("m", s_msg);
}

// ---------------------------------------------------------------------------
// Parser: msg truncation to 160 bytes
// ---------------------------------------------------------------------------

void test_bb_log_event_parse_msg_truncation(void)
{
    // Build a line with a 200-char msg
    char line[300];
    // "I (0) t: " = 9 chars, then 200 'a'
    memcpy(line, "I (0) t: ", 9);
    memset(line + 9, 'a', 200);
    line[209] = '\0';

    char level;
    char tag[16];
    char msg[256];
    bb_log_event_parse(line, strlen(line), &level, tag, sizeof(tag), msg, sizeof(msg));

    TEST_ASSERT_EQUAL_CHAR('I', level);
    TEST_ASSERT_EQUAL_STRING("t", tag);
    // msg must be truncated to exactly 160 'a's
    TEST_ASSERT_EQUAL_INT(160, (int)strlen(msg));
    for (int i = 0; i < 160; i++) {
        TEST_ASSERT_EQUAL_CHAR('a', msg[i]);
    }
}

// ---------------------------------------------------------------------------
// Parser: tag truncation (cap smaller than tag)
// ---------------------------------------------------------------------------

void test_bb_log_event_parse_tag_truncation(void)
{
    char level;
    char tag[5];  // only 4 chars + NUL
    char msg[64];
    bb_log_event_parse("I (1) longtag: hi", 17, &level, tag, sizeof(tag), msg, sizeof(msg));
    TEST_ASSERT_EQUAL_CHAR('I', level);
    TEST_ASSERT_EQUAL_STRING("long", tag);  // truncated to 4 chars
    TEST_ASSERT_EQUAL_STRING("hi", msg);
}

// ---------------------------------------------------------------------------
// Parser: msg with colon inside (": " appears in both tag and msg)
// ---------------------------------------------------------------------------

void test_bb_log_event_parse_colon_in_msg(void)
{
    parse("I (1) tag: url: http://x");
    TEST_ASSERT_EQUAL_CHAR('I', s_level);
    TEST_ASSERT_EQUAL_STRING("tag", s_tag);
    TEST_ASSERT_EQUAL_STRING("url: http://x", s_msg);
}

// ---------------------------------------------------------------------------
// msg_out cap=0 does not crash
// ---------------------------------------------------------------------------

void test_bb_log_event_parse_zero_cap(void)
{
    char level;
    char tag[16];
    bb_log_event_parse("I (1) t: msg", 12, &level, tag, sizeof(tag), NULL, 0);
    TEST_ASSERT_EQUAL_CHAR('I', level);
    TEST_ASSERT_EQUAL_STRING("t", tag);
}
