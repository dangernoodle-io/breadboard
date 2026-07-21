// Host tests for bb_log_event_line_wire (the internal per-log-line
// {"ts","level","tag","msg"} JSON build, migrated off cJSON to
// bb_serialize). Renders bb_log_event_line_wire_desc via
// bb_serialize_json_render() (the same one-shot entry point
// s_forwarder_task drives in production) and asserts the resulting JSON
// string exactly, byte for byte.

#include "unity.h"

#include "../../components/bb_log_event/bb_log_event_line_wire_priv.h"
#include "../../components/bb_log_event/include/bb_log_event_wire.h"

#include "bb_serialize_json.h"
#include "bb_str.h"

#include <stdio.h>
#include <string.h>

// Production render-scratch size (bb_log_event_line_wire_priv.h) -- the
// same worst-case constant s_forwarder_task (platform/espidf/bb_log_event/
// bb_log_event.c) renders into. Sizing tests to anything smaller (the old
// 512 here, production's old 220) would miss the overflow this test guards
// against.
#define RENDER_BUF_BYTES BB_LOG_EVENT_LINE_JSON_MAX

static void line_render(int64_t ts, char level, const char *tag, const char *msg,
                         char *out_buf)
{
    bb_log_event_line_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.ts = ts;
    char level_str[2] = { level, '\0' };
    strncpy(snap.level, level_str, sizeof(snap.level) - 1);
    strncpy(snap.tag, tag, sizeof(snap.tag) - 1);
    strncpy(snap.msg, msg, sizeof(snap.msg) - 1);

    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_log_event_line_wire_desc, &snap,
                                            out_buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// 1. Normal row, large realistic ts -- confirms int64 renders as an exact
// integer, never a double/scientific form.
// ---------------------------------------------------------------------------

void test_log_event_line_wire_expected_json_normal_row(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1721574000000LL, 'I', "wifi", "connected", buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":1721574000000,\"level\":\"I\",\"tag\":\"wifi\",\"msg\":\"connected\"}", buf);
}

// ---------------------------------------------------------------------------
// 2. Each level value.
// ---------------------------------------------------------------------------

void test_log_event_line_wire_expected_json_level_info(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1, 'I', "t", "m", buf);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}", buf);
}

void test_log_event_line_wire_expected_json_level_warn(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1, 'W', "t", "m", buf);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"level\":\"W\",\"tag\":\"t\",\"msg\":\"m\"}", buf);
}

void test_log_event_line_wire_expected_json_level_error(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1, 'E', "t", "m", buf);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"level\":\"E\",\"tag\":\"t\",\"msg\":\"m\"}", buf);
}

void test_log_event_line_wire_expected_json_level_debug(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1, 'D', "t", "m", buf);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"level\":\"D\",\"tag\":\"t\",\"msg\":\"m\"}", buf);
}

void test_log_event_line_wire_expected_json_level_verbose(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1, 'V', "t", "m", buf);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"level\":\"V\",\"tag\":\"t\",\"msg\":\"m\"}", buf);
}

void test_log_event_line_wire_expected_json_level_fallback(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1, '?', "t", "m", buf);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"level\":\"?\",\"tag\":\"t\",\"msg\":\"m\"}", buf);
}

// ---------------------------------------------------------------------------
// 3. Empty msg.
// ---------------------------------------------------------------------------

void test_log_event_line_wire_expected_json_empty_msg(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1, 'I', "t", "", buf);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"\"}", buf);
}

// ---------------------------------------------------------------------------
// 4. Escaped msg -- quote + backslash + a control character (0x01). The
// literal is split into adjacent string-literal segments
// ("Ctrl\x01" "Char") to avoid C's hex-escape greedy-parse bug (\x01C would
// otherwise be parsed as a single, invalid multi-digit hex escape).
// ---------------------------------------------------------------------------

void test_log_event_line_wire_expected_json_escaped_msg(void)
{
    char buf[RENDER_BUF_BYTES];
    line_render(1, 'I', "t", "Weird\"Msg\\Ctrl\x01" "Char", buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":1,\"level\":\"I\",\"tag\":\"t\","
        "\"msg\":\"Weird\\\"Msg\\\\Ctrl\\u0001Char\"}", buf);
}

// ---------------------------------------------------------------------------
// 5. Max-length tag (47 chars) + msg (167 chars) -- one byte short of each
// buffer's full char[N] capacity (48/168), leaving room for the NUL.
// ---------------------------------------------------------------------------

void test_log_event_line_wire_expected_json_max_length_tag_and_msg(void)
{
    char tag[48];
    memset(tag, 'A', sizeof(tag) - 1);
    tag[sizeof(tag) - 1] = '\0';

    char msg[168];
    memset(msg, 'B', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';

    char buf[RENDER_BUF_BYTES];
    line_render(1, 'I', tag, msg, buf);

    char expected[RENDER_BUF_BYTES];
    int off = snprintf(expected, sizeof(expected), "{\"ts\":1,\"level\":\"I\",\"tag\":\"");
    off += snprintf(expected + off, sizeof(expected) - (size_t)off, "%s", tag);
    off += snprintf(expected + off, sizeof(expected) - (size_t)off, "\",\"msg\":\"");
    off += snprintf(expected + off, sizeof(expected) - (size_t)off, "%s", msg);
    snprintf(expected + off, sizeof(expected) - (size_t)off, "\"}");

    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

// ---------------------------------------------------------------------------
// 5b. Max-length-both parity -- tag(47)+msg(167), the SAME worst case as
// test #5 above, but this is the one that would have FAILED against the
// old production sizing (render_buf[BB_LOG_EVENT_LOG_TEXT_MAX] == 220):
// renders into a buffer sized to BB_LOG_EVENT_LINE_JSON_MAX (production's
// real render-scratch size) and asserts (a) bb_serialize_json_render()
// returns BB_OK -- never BB_ERR_NO_SPACE -- then (b) the subsequent
// bb_strlcpy into a 220-byte buffer (the "log" stash's real size)
// truncates to 219 chars + NUL, documenting the truncate-not-drop parity
// with the old cJSON-then-bb_strlcpy path.
// ---------------------------------------------------------------------------

void test_log_event_line_wire_max_length_both_renders_and_stash_truncates(void)
{
    char tag[48];
    memset(tag, 'A', sizeof(tag) - 1);
    tag[sizeof(tag) - 1] = '\0';

    char msg[168];
    memset(msg, 'B', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';

    char buf[BB_LOG_EVENT_LINE_JSON_MAX];
    bb_log_event_line_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.ts = 1;
    bb_strlcpy(snap.level, "I", sizeof(snap.level));
    bb_strlcpy(snap.tag, tag, sizeof(snap.tag));
    bb_strlcpy(snap.msg, msg, sizeof(snap.msg));

    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_log_event_line_wire_desc, &snap,
                                            buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    char expected[BB_LOG_EVENT_LINE_JSON_MAX];
    int off = snprintf(expected, sizeof(expected), "{\"ts\":1,\"level\":\"I\",\"tag\":\"");
    off += snprintf(expected + off, sizeof(expected) - (size_t)off, "%s", tag);
    off += snprintf(expected + off, sizeof(expected) - (size_t)off, "\",\"msg\":\"");
    off += snprintf(expected + off, sizeof(expected) - (size_t)off, "%s", msg);
    snprintf(expected + off, sizeof(expected) - (size_t)off, "\"}");
    TEST_ASSERT_EQUAL_STRING(expected, buf);

    char stash[BB_LOG_EVENT_LOG_TEXT_MAX];
    size_t src_len = bb_strlcpy(stash, buf, sizeof(stash));
    TEST_ASSERT_EQUAL_UINT(strlen(expected), src_len);
    TEST_ASSERT_EQUAL_UINT(BB_LOG_EVENT_LOG_TEXT_MAX - 1, strlen(stash));

    char expected_stash[BB_LOG_EVENT_LOG_TEXT_MAX];
    memcpy(expected_stash, expected, BB_LOG_EVENT_LOG_TEXT_MAX - 1);
    expected_stash[BB_LOG_EVENT_LOG_TEXT_MAX - 1] = '\0';
    TEST_ASSERT_EQUAL_STRING(expected_stash, stash);
}

// ---------------------------------------------------------------------------
// 6. Row-field-count invariant -- guards bb_log_event_line_wire_n_fields
// against drift, same precedent as
// test_ota_validator_partitions_wire_row_field_count_matches.
// ---------------------------------------------------------------------------

void test_log_event_line_wire_row_field_count_matches(void)
{
    TEST_ASSERT_EQUAL_UINT16(4, bb_log_event_line_wire_n_fields);
}
