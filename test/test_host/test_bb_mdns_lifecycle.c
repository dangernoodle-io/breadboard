#include "unity.h"
#include "bb_mdns_lifecycle.h"

// Adapter counters for testing
static int s_init_calls = 0;
static int s_free_calls = 0;
static int s_apply_calls = 0;
static int s_bye_calls = 0;

static int s_init_return = 0;
static int s_apply_return = 0;

static int s_call_seq = 0;
static int s_bye_seq = 0;
static int s_free_seq = 0;

static int fake_init(void)
{
    s_init_calls++;
    return s_init_return;
}

static void fake_free(void)
{
    s_free_calls++;
    s_free_seq = ++s_call_seq;
}

static int fake_apply(void)
{
    s_apply_calls++;
    return s_apply_return;
}

static void fake_bye(void)
{
    s_bye_calls++;
    s_bye_seq = ++s_call_seq;
}

static const bb_mdns_lifecycle_adapter_t adapter = {
    .mdns_init = fake_init,
    .mdns_free = fake_free,
    .mdns_apply_announce = fake_apply,
    .mdns_send_bye = fake_bye,
};

static bb_mdns_lifecycle_state_t s_st;

void bb_mdns_lifecycle_test_reset(void)
{
    s_init_calls = 0;
    s_free_calls = 0;
    s_apply_calls = 0;
    s_bye_calls = 0;
    s_init_return = 0;
    s_apply_return = 0;
    s_call_seq = 0;
    s_bye_seq = 0;
    s_free_seq = 0;
    bb_mdns_lifecycle_reset(&s_st);
}

void test_bb_mdns_lifecycle_start_when_not_started(void)
{
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_start(&s_st, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_OK, res);
    TEST_ASSERT_EQUAL(1, s_init_calls);
    TEST_ASSERT_TRUE(bb_mdns_lifecycle_is_started(&s_st));
}

void test_bb_mdns_lifecycle_start_when_already_started_is_noop(void)
{
    bb_mdns_lifecycle_start(&s_st, &adapter);
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_start(&s_st, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_ALREADY_STARTED, res);
    TEST_ASSERT_EQUAL(1, s_init_calls);
    TEST_ASSERT_TRUE(bb_mdns_lifecycle_is_started(&s_st));
}

void test_bb_mdns_lifecycle_start_init_failure_keeps_state_unstarted(void)
{
    s_init_return = -1;
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_start(&s_st, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_INIT_FAILED, res);
    TEST_ASSERT_EQUAL(1, s_init_calls);
    TEST_ASSERT_FALSE(bb_mdns_lifecycle_is_started(&s_st));
}

void test_bb_mdns_lifecycle_stop_when_started_sends_bye_then_free(void)
{
    bb_mdns_lifecycle_start(&s_st, &adapter);
    s_call_seq = 0;  // Reset seq for stop test
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_stop(&s_st, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_OK, res);
    TEST_ASSERT_EQUAL(1, s_bye_calls);
    TEST_ASSERT_EQUAL(1, s_free_calls);
    TEST_ASSERT_FALSE(bb_mdns_lifecycle_is_started(&s_st));
    // Verify bye was called before free
    TEST_ASSERT_LESS_THAN(s_free_seq, s_bye_seq);
}

void test_bb_mdns_lifecycle_stop_when_not_started_is_noop(void)
{
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_stop(&s_st, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_NOT_STARTED, res);
    TEST_ASSERT_EQUAL(0, s_free_calls);
    TEST_ASSERT_EQUAL(0, s_bye_calls);
}

void test_bb_mdns_lifecycle_announce_when_started_calls_apply(void)
{
    bb_mdns_lifecycle_start(&s_st, &adapter);
    s_apply_calls = 0;  // Reset
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_announce(&s_st, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_OK, res);
    TEST_ASSERT_EQUAL(1, s_apply_calls);
    TEST_ASSERT_FALSE(s_st.announce_dirty);
}

void test_bb_mdns_lifecycle_announce_when_stopped_marks_dirty(void)
{
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_announce(&s_st, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_NOT_STARTED, res);
    TEST_ASSERT_EQUAL(0, s_apply_calls);
    TEST_ASSERT_TRUE(s_st.announce_dirty);

    // Now start and verify dirty was flushed
    s_apply_calls = 0;
    bb_mdns_lifecycle_start(&s_st, &adapter);
    TEST_ASSERT_EQUAL(1, s_apply_calls);
    TEST_ASSERT_FALSE(s_st.announce_dirty);
}

void test_bb_mdns_lifecycle_restart_cycle(void)
{
    // Start
    bb_mdns_lifecycle_start(&s_st, &adapter);
    TEST_ASSERT_EQUAL(1, s_init_calls);

    // Stop
    bb_mdns_lifecycle_stop(&s_st, &adapter);
    TEST_ASSERT_EQUAL(1, s_bye_calls);
    TEST_ASSERT_EQUAL(1, s_free_calls);

    // Start again
    bb_mdns_lifecycle_start(&s_st, &adapter);
    TEST_ASSERT_EQUAL(2, s_init_calls);
    TEST_ASSERT_TRUE(bb_mdns_lifecycle_is_started(&s_st));
}

void test_bb_mdns_lifecycle_invalid_args(void)
{
    // start with NULL st
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_start(NULL, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_INVALID_ARG, res);
    TEST_ASSERT_EQUAL(0, s_init_calls);

    // start with NULL adapter
    res = bb_mdns_lifecycle_start(&s_st, NULL);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_INVALID_ARG, res);
    TEST_ASSERT_EQUAL(0, s_init_calls);

    // stop with NULL st
    res = bb_mdns_lifecycle_stop(NULL, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_INVALID_ARG, res);

    // stop with NULL adapter
    res = bb_mdns_lifecycle_stop(&s_st, NULL);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_INVALID_ARG, res);

    // announce with NULL st
    res = bb_mdns_lifecycle_announce(NULL, &adapter);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_INVALID_ARG, res);

    // announce with NULL adapter
    res = bb_mdns_lifecycle_announce(&s_st, NULL);
    TEST_ASSERT_EQUAL(BB_MDNS_LC_INVALID_ARG, res);

    // is_started with NULL st
    bool started = bb_mdns_lifecycle_is_started(NULL);
    TEST_ASSERT_FALSE(started);

    // mark_dirty NULL is no-op; non-NULL sets the flag
    bb_mdns_lifecycle_mark_dirty(NULL);
    bb_mdns_lifecycle_mark_dirty(&s_st);
    TEST_ASSERT_TRUE(s_st.announce_dirty);

    // reset(NULL) is a no-op
    bb_mdns_lifecycle_reset(NULL);
}

