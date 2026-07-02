// Tests for bb_mqtt_reasm_step (B1-487 HIGH-1): the pure fragmented-payload
// reassembly state machine shared verbatim by the espidf MQTT_EVENT_DATA glue
// and this file — no mirror/duplicate implementation.
#include "unity.h"
#include "bb_mqtt_reassemble.h"

#include <string.h>

#define TEST_BUF_CAP 16

static char                   s_buf[TEST_BUF_CAP];
static bb_mqtt_reasm_state_t  s_st;

static void reasm_setup(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    memset(&s_st, 0, sizeof(s_st));
    s_st.buf     = s_buf;
    s_st.buf_cap = sizeof(s_buf);
    bb_mqtt_reasm_reset(&s_st);
}

void test_bb_mqtt_reasm_single_shot_completes_immediately(void)
{
    reasm_setup();
    bool complete = bb_mqtt_reasm_step(&s_st, "t/single", strlen("t/single"),
                                        5, 0, "hello", 5);

    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_STRING("t/single", s_st.topic);
    TEST_ASSERT_EQUAL_INT(5, (int)s_st.len);
    TEST_ASSERT_EQUAL_MEMORY("hello", s_st.buf, 5);
    TEST_ASSERT_FALSE(s_st.active);
}

void test_bb_mqtt_reasm_three_fragment_concat(void)
{
    reasm_setup();
    // total = 9, delivered as "abc" + "def" + "ghi"
    bool c1 = bb_mqtt_reasm_step(&s_st, "t/multi", strlen("t/multi"), 9, 0, "abc", 3);
    TEST_ASSERT_FALSE(c1);
    bool c2 = bb_mqtt_reasm_step(&s_st, NULL, 0, 9, 3, "def", 3);
    TEST_ASSERT_FALSE(c2);
    bool c3 = bb_mqtt_reasm_step(&s_st, NULL, 0, 9, 6, "ghi", 3);
    TEST_ASSERT_TRUE(c3);

    TEST_ASSERT_EQUAL_STRING("t/multi", s_st.topic);
    TEST_ASSERT_EQUAL_INT(9, (int)s_st.len);
    TEST_ASSERT_EQUAL_MEMORY("abcdefghi", s_st.buf, 9);
    TEST_ASSERT_FALSE(s_st.active);
}

void test_bb_mqtt_reasm_total_exceeds_buffer_is_dropped(void)
{
    reasm_setup();
    // buf_cap is 16; declare a total of 64 — should be rejected outright.
    bool complete = bb_mqtt_reasm_step(&s_st, "t/big", strlen("t/big"),
                                        64, 0, "xxxxxxxx", 8);
    TEST_ASSERT_FALSE(complete);
    TEST_ASSERT_FALSE(s_st.active);

    // Trailing fragment of the same (dropped) message must also be ignored.
    bool trailing = bb_mqtt_reasm_step(&s_st, NULL, 0, 64, 8, "yyyyyyyy", 8);
    TEST_ASSERT_FALSE(trailing);
}

void test_bb_mqtt_reasm_zero_length_payload_completes(void)
{
    reasm_setup();
    bool complete = bb_mqtt_reasm_step(&s_st, "t/empty", strlen("t/empty"),
                                        0, 0, NULL, 0);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_STRING("t/empty", s_st.topic);
    TEST_ASSERT_EQUAL_INT(0, (int)s_st.len);
}

void test_bb_mqtt_reasm_mid_message_overflow_is_dropped(void)
{
    reasm_setup();
    // Declared total (10) fits the buffer, but the broker delivers more
    // bytes than the declared total mid-stream — must be dropped, not
    // silently accepted or overrun the buffer.
    bool c1 = bb_mqtt_reasm_step(&s_st, "t/overflow", strlen("t/overflow"), 10, 0, "12345", 5);
    TEST_ASSERT_FALSE(c1);
    bool c2 = bb_mqtt_reasm_step(&s_st, NULL, 0, 10, 5, "678901234", 9); // 5+9=14 > total(10)
    TEST_ASSERT_FALSE(c2);
    TEST_ASSERT_FALSE(s_st.active);

    // A further trailing fragment for the same dropped message is ignored.
    bool c3 = bb_mqtt_reasm_step(&s_st, NULL, 0, 10, 14, "z", 1);
    TEST_ASSERT_FALSE(c3);
}

void test_bb_mqtt_reasm_no_buffer_is_safe_noop(void)
{
    bb_mqtt_reasm_state_t st;
    memset(&st, 0, sizeof(st));  // buf == NULL, buf_cap == 0
    bool complete = bb_mqtt_reasm_step(&st, "t", 1, 5, 0, "hello", 5);
    TEST_ASSERT_FALSE(complete);
}

void test_bb_mqtt_reasm_second_message_reuses_state_cleanly(void)
{
    reasm_setup();
    bb_mqtt_reasm_step(&s_st, "t/first", strlen("t/first"), 3, 0, "abc", 3);
    bool complete = bb_mqtt_reasm_step(&s_st, "t/second", strlen("t/second"), 3, 0, "xyz", 3);

    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_STRING("t/second", s_st.topic);
    TEST_ASSERT_EQUAL_MEMORY("xyz", s_st.buf, 3);
}

void test_bb_mqtt_reasm_reset_null_is_safe_noop(void)
{
    bb_mqtt_reasm_reset(NULL);  // must not crash
}

void test_bb_mqtt_reasm_null_state_is_safe_noop(void)
{
    bool complete = bb_mqtt_reasm_step(NULL, "t", 1, 5, 0, "hello", 5);
    TEST_ASSERT_FALSE(complete);
}

void test_bb_mqtt_reasm_zero_cap_with_buffer_is_safe_noop(void)
{
    bb_mqtt_reasm_state_t st;
    char                   buf[TEST_BUF_CAP];
    memset(&st, 0, sizeof(st));
    st.buf     = buf;  // non-NULL buffer, but zero capacity
    st.buf_cap = 0;
    bool complete = bb_mqtt_reasm_step(&st, "t", 1, 5, 0, "hello", 5);
    TEST_ASSERT_FALSE(complete);
}

void test_bb_mqtt_reasm_null_topic_at_offset_zero_yields_empty_topic(void)
{
    reasm_setup();
    bool complete = bb_mqtt_reasm_step(&s_st, NULL, 0, 3, 0, "abc", 3);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_STRING("", s_st.topic);
}

void test_bb_mqtt_reasm_zero_length_topic_yields_empty_topic(void)
{
    reasm_setup();
    bool complete = bb_mqtt_reasm_step(&s_st, "t/ignored", 0, 3, 0, "abc", 3);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_STRING("", s_st.topic);
}

void test_bb_mqtt_reasm_topic_truncated_at_max(void)
{
    reasm_setup();
    char long_topic[BB_MQTT_REASM_TOPIC_MAX + 32];
    memset(long_topic, 'x', sizeof(long_topic));
    long_topic[sizeof(long_topic) - 1] = '\0';

    bool complete = bb_mqtt_reasm_step(&s_st, long_topic, strlen(long_topic), 1, 0, "a", 1);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_INT(BB_MQTT_REASM_TOPIC_MAX - 1, (int)strlen(s_st.topic));
}

void test_bb_mqtt_reasm_buf_cap_exceeded_mid_message_is_dropped(void)
{
    reasm_setup();
    // total == buf_cap (16), so the first-fragment total>buf_cap guard
    // passes; the second fragment then overruns buf_cap directly (left
    // side of the OR at bb_mqtt_reasm_step's overflow check), independent
    // of whether it also overruns total.
    bool c1 = bb_mqtt_reasm_step(&s_st, "t/cap", strlen("t/cap"), TEST_BUF_CAP, 0,
                                  "0123456789", 10);
    TEST_ASSERT_FALSE(c1);
    bool c2 = bb_mqtt_reasm_step(&s_st, NULL, 0, TEST_BUF_CAP, 10, "0123456789", 10);
    TEST_ASSERT_FALSE(c2);
    TEST_ASSERT_FALSE(s_st.active);
}

void test_bb_mqtt_reasm_zero_length_continuation_fragment_skips_copy(void)
{
    reasm_setup();
    bool c1 = bb_mqtt_reasm_step(&s_st, "t/cont", strlen("t/cont"), 10, 0, "abc", 3);
    TEST_ASSERT_FALSE(c1);
    // Non-NULL pointer, but zero-length continuation fragment.
    bool c2 = bb_mqtt_reasm_step(&s_st, NULL, 0, 10, 3, "", 0);
    TEST_ASSERT_FALSE(c2);
    TEST_ASSERT_EQUAL_INT(3, (int)s_st.len);
    TEST_ASSERT_EQUAL_MEMORY("abc", s_st.buf, 3);
}
