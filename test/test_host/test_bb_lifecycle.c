#include "unity.h"
#include "bb_lifecycle.h"

#include <stdio.h>
#include <string.h>

// Test-local reason strings (open-vocabulary -- bb_lifecycle ships no
// reason constants of its own; anonymized, no real-world tie-in).
#define TEST_R_OTA     "ota"
#define TEST_R_THERMAL "thermal"

// TEST_MAX_SERVICES/MAX_OBSERVERS are only bridged inside
// bb_lifecycle.c (not the public header, unlike MAX_REASONS which the
// header needs for the BB_LIFECYCLE_INHIBIT_WORDS math) -- mirror their
// Kconfig C defaults here for the capacity-overflow tests.
#define TEST_MAX_SERVICES  8
#define TEST_MAX_OBSERVERS 8

// ---------------------------------------------------------------------------
// Observer + emit-sink capture (statics, cleared in setUp per test)
// ---------------------------------------------------------------------------

static int                    s_obs_calls;
static bb_lifecycle_event_t   s_obs_last;
static void                  *s_obs_last_user;
static int                    s_obs_order[4];
static int                    s_obs_order_count;

static void reset_capture(void)
{
    s_obs_calls = 0;
    memset(&s_obs_last, 0, sizeof(s_obs_last));
    s_obs_last_user = NULL;
    memset(s_obs_order, 0, sizeof(s_obs_order));
    s_obs_order_count = 0;
}

static void observer_a(const bb_lifecycle_event_t *evt, void *user)
{
    s_obs_calls++;
    s_obs_last = *evt;
    s_obs_last_user = user;
    if (s_obs_order_count < 4) s_obs_order[s_obs_order_count++] = 1;
}

static void observer_b(const bb_lifecycle_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    if (s_obs_order_count < 4) s_obs_order[s_obs_order_count++] = 2;
}

// Reentrancy: an observer calling a query (is_paused) must not deadlock.
static bool s_reentrant_result;
static void observer_reentrant(const bb_lifecycle_event_t *evt, void *user)
{
    bb_lifecycle_svc_t svc = (bb_lifecycle_svc_t)(intptr_t)user;
    (void)evt;
    s_reentrant_result = bb_lifecycle_is_paused(svc);
    s_obs_calls++;
}

static char   s_emit_topic[32];
static int32_t s_emit_id;
static bb_lifecycle_event_t s_emit_payload;
static int    s_emit_calls;
static void  *s_emit_ctx;

static void reset_emit_capture(void)
{
    memset(s_emit_topic, 0, sizeof(s_emit_topic));
    s_emit_id = -999;
    memset(&s_emit_payload, 0, sizeof(s_emit_payload));
    s_emit_calls = 0;
    s_emit_ctx = NULL;
}

static void stub_emit(void *ctx, const char *topic, int32_t id, const void *payload, size_t size)
{
    s_emit_calls++;
    s_emit_ctx = ctx;
    strncpy(s_emit_topic, topic, sizeof(s_emit_topic) - 1);
    s_emit_id = id;
    if (payload && size == sizeof(s_emit_payload)) {
        memcpy(&s_emit_payload, payload, sizeof(s_emit_payload));
    }
}

// test_main.c owns the single shared setUp()/tearDown() for the whole host
// test binary (one setUp per translation unit is not possible -- every
// test_*.c file's tests are RUN_TEST'd from test_main.c); this file exposes
// its per-test reset as a plain external function that test_main.c's setUp()
// calls by name (matches e.g. bb_ota_hooks_test_reset()'s convention).
void test_bb_lifecycle_reset_local(void)
{
    bb_lifecycle_reset_for_test();
    reset_capture();
    reset_emit_capture();
    s_reentrant_result = false;
}

// ---------------------------------------------------------------------------
// autoinit (sole codegen/handwire entry point)
// ---------------------------------------------------------------------------

void test_bb_lifecycle_autoinit_returns_ok(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_autoinit());

    // lock/registry usable afterward
    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc = BB_LIFECYCLE_SVC_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc));
    TEST_ASSERT_NOT_EQUAL(BB_LIFECYCLE_SVC_INVALID, svc);
}

// ---------------------------------------------------------------------------
// register / find / name
// ---------------------------------------------------------------------------

void test_bb_lifecycle_register_starts_stopped(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc = BB_LIFECYCLE_SVC_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc));
    TEST_ASSERT_NOT_EQUAL(BB_LIFECYCLE_SVC_INVALID, svc);
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_state(svc));
}

void test_bb_lifecycle_find_hits_registered_service(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc = BB_LIFECYCLE_SVC_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc));

    bb_lifecycle_svc_t found = BB_LIFECYCLE_SVC_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_find("svc-a", &found));
    TEST_ASSERT_EQUAL(svc, found);
}

void test_bb_lifecycle_find_miss_returns_not_found(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_svc_t found = BB_LIFECYCLE_SVC_INVALID;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_lifecycle_find("nope", &found));
}

void test_bb_lifecycle_find_miss_against_existing_services(void)
{
    // Distinct from the empty-table miss above -- exercises the loop's
    // strcmp-false branch against a real (non-matching) registered entry.
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    bb_lifecycle_svc_t found = BB_LIFECYCLE_SVC_INVALID;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_lifecycle_find("svc-b", &found));
}

void test_bb_lifecycle_find_null_args_invalid_arg(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_svc_t found;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_find(NULL, &found));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_find("svc-a", NULL));
}

void test_bb_lifecycle_name_returns_registered_name(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc = BB_LIFECYCLE_SVC_INVALID;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc));
    TEST_ASSERT_EQUAL_STRING("svc-a", bb_lifecycle_name(svc));
}

void test_bb_lifecycle_name_bad_handle_returns_empty(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL_STRING("", bb_lifecycle_name((bb_lifecycle_svc_t)999));
    TEST_ASSERT_EQUAL_STRING("", bb_lifecycle_name(BB_LIFECYCLE_SVC_INVALID));
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

void test_bb_lifecycle_start_transitions_to_running(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_start(svc));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_state(svc));
    TEST_ASSERT_FALSE(bb_lifecycle_is_paused(svc));
}

void test_bb_lifecycle_stop_transitions_to_stopped(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_stop(svc));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_state(svc));
}

void test_bb_lifecycle_start_bad_handle_not_found(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_lifecycle_start((bb_lifecycle_svc_t)999));
}

void test_bb_lifecycle_stop_bad_handle_not_found(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_lifecycle_stop((bb_lifecycle_svc_t)999));
}

// ---------------------------------------------------------------------------
// bad-handle queries (is_paused / state / version / inhibit_words)
// ---------------------------------------------------------------------------

void test_bb_lifecycle_is_paused_bad_handle_returns_false(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_FALSE(bb_lifecycle_is_paused((bb_lifecycle_svc_t)999));
    TEST_ASSERT_FALSE(bb_lifecycle_is_paused(BB_LIFECYCLE_SVC_INVALID));
}

void test_bb_lifecycle_state_bad_handle_returns_stopped(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_state((bb_lifecycle_svc_t)999));
}

void test_bb_lifecycle_version_bad_handle_returns_zero(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL_UINT32(0, bb_lifecycle_version((bb_lifecycle_svc_t)999));
}

void test_bb_lifecycle_inhibit_words_bad_args_return_zero(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    uint32_t words[BB_LIFECYCLE_INHIBIT_WORDS] = {0};
    TEST_ASSERT_EQUAL((size_t)0, bb_lifecycle_inhibit_words(svc, NULL, BB_LIFECYCLE_INHIBIT_WORDS));
    TEST_ASSERT_EQUAL((size_t)0, bb_lifecycle_inhibit_words(svc, words, 0));
    TEST_ASSERT_EQUAL((size_t)0, bb_lifecycle_inhibit_words((bb_lifecycle_svc_t)999, words, BB_LIFECYCLE_INHIBIT_WORDS));
}

// ---------------------------------------------------------------------------
// pause_assert / multi-inhibit / intern reuse / stop-clears
// ---------------------------------------------------------------------------

void test_bb_lifecycle_pause_assert_transitions_to_paused(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, TEST_R_OTA));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_PAUSED, bb_lifecycle_state(svc));
    TEST_ASSERT_TRUE(bb_lifecycle_is_paused(svc));
}

void test_bb_lifecycle_multi_inhibit_coexistence(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, TEST_R_OTA));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, TEST_R_THERMAL));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_PAUSED, bb_lifecycle_state(svc));

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_clear(svc, TEST_R_OTA));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_PAUSED, bb_lifecycle_state(svc)); // thermal still set

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_clear(svc, TEST_R_THERMAL));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_state(svc));
}

void test_bb_lifecycle_intern_reuse_same_bit(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg_a = { .name = "svc-a" };
    bb_lifecycle_config_t cfg_b = { .name = "svc-b" };
    bb_lifecycle_svc_t a, b;
    bb_lifecycle_register(&cfg_a, &a);
    bb_lifecycle_register(&cfg_b, &b);
    bb_lifecycle_start(a);
    bb_lifecycle_start(b);

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(a, TEST_R_OTA));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(b, TEST_R_OTA));

    uint32_t words_a[BB_LIFECYCLE_INHIBIT_WORDS] = {0};
    uint32_t words_b[BB_LIFECYCLE_INHIBIT_WORDS] = {0};
    bb_lifecycle_inhibit_words(a, words_a, BB_LIFECYCLE_INHIBIT_WORDS);
    bb_lifecycle_inhibit_words(b, words_b, BB_LIFECYCLE_INHIBIT_WORDS);
    TEST_ASSERT_EQUAL_UINT32(words_a[0], words_b[0]); // same interned bit -- global intern

    // idempotent re-assert: no notify, no version bump
    uint32_t v_before = bb_lifecycle_version(a);
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(a, TEST_R_OTA));
    TEST_ASSERT_EQUAL_UINT32(v_before, bb_lifecycle_version(a));

    // one clear resumes
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_clear(a, TEST_R_OTA));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_state(a));

    // clearing again: the reason IS interned (svc b still has it asserted),
    // but is already clear on svc a -- idempotent no-op, no version bump.
    uint32_t v_after_clear = bb_lifecycle_version(a);
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_clear(a, TEST_R_OTA));
    TEST_ASSERT_EQUAL_UINT32(v_after_clear, bb_lifecycle_version(a));
}

void test_bb_lifecycle_stop_clears_inhibits(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);
    bb_lifecycle_pause_assert(svc, TEST_R_OTA);
    TEST_ASSERT_TRUE(bb_lifecycle_is_paused(svc));

    bb_lifecycle_stop(svc);
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_state(svc));

    bb_lifecycle_start(svc);
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_state(svc)); // inhibits gone
    TEST_ASSERT_FALSE(bb_lifecycle_is_paused(svc));
}

// ---------------------------------------------------------------------------
// dup / capacity / arg errors
// ---------------------------------------------------------------------------

void test_bb_lifecycle_register_dup_name_conflict(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc_a, svc_b;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc_a));
    TEST_ASSERT_EQUAL(BB_ERR_CONFLICT, bb_lifecycle_register(&cfg, &svc_b));
}

void test_bb_lifecycle_register_capacity_overflow_no_space(void)
{
    test_bb_lifecycle_reset_local();

    char name[16];
    bb_lifecycle_svc_t svc;
    bb_err_t err = BB_OK;
    size_t i = 0;
    for (; i < TEST_MAX_SERVICES; i++) {
        snprintf(name, sizeof(name), "svc-%zu", i);
        bb_lifecycle_config_t cfg = { .name = name };
        err = bb_lifecycle_register(&cfg, &svc);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    bb_lifecycle_config_t overflow_cfg = { .name = "one-too-many" };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_lifecycle_register(&overflow_cfg, &svc));
    TEST_ASSERT_EQUAL((size_t)TEST_MAX_SERVICES, bb_lifecycle_count());
}

void test_bb_lifecycle_pause_assert_null_reason_invalid_arg(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_pause_assert(svc, NULL));
}

void test_bb_lifecycle_pause_assert_bad_handle_not_found(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_lifecycle_pause_assert((bb_lifecycle_svc_t)999, TEST_R_OTA));
}

void test_bb_lifecycle_register_null_args_invalid_arg(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_svc_t svc;
    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_register(NULL, &svc));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_register(&cfg, NULL));
    bb_lifecycle_config_t cfg_null_name = { .name = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_register(&cfg_null_name, &svc));
}

void test_bb_lifecycle_intern_overflow_33rd_reason_no_space(void)
{
    test_bb_lifecycle_reset_local();

    // Default CONFIG_BB_LIFECYCLE_MAX_REASONS is 32; a 33rd distinct reason
    // must fail with NO_SPACE without disturbing the already-interned 32.
    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    char reason[16];
    for (int i = 0; i < CONFIG_BB_LIFECYCLE_MAX_REASONS; i++) {
        snprintf(reason, sizeof(reason), "r%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, reason));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_lifecycle_pause_assert(svc, "one-too-many"));
}

void test_bb_lifecycle_reason_name_reverse_lookup(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);
    bb_lifecycle_pause_assert(svc, TEST_R_OTA);

    uint32_t words[BB_LIFECYCLE_INHIBIT_WORDS] = {0};
    bb_lifecycle_inhibit_words(svc, words, BB_LIFECYCLE_INHIBIT_WORDS);
    TEST_ASSERT_TRUE(bb_lifecycle_word_test_for_test(words, BB_LIFECYCLE_INHIBIT_WORDS, 0));
    TEST_ASSERT_EQUAL_STRING(TEST_R_OTA, bb_lifecycle_reason_name(0));
}

void test_bb_lifecycle_reason_name_unused_bit_empty(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL_STRING("", bb_lifecycle_reason_name(0));
    TEST_ASSERT_EQUAL_STRING("", bb_lifecycle_reason_name(254));
}

// ---------------------------------------------------------------------------
// idempotent no-ops (stop-when-stopped, start-when-started, clear-never-interned)
// ---------------------------------------------------------------------------

void test_bb_lifecycle_stop_when_stopped_noop(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    uint32_t v_before = bb_lifecycle_version(svc);
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_stop(svc));
    TEST_ASSERT_EQUAL_UINT32(v_before, bb_lifecycle_version(svc));
}

void test_bb_lifecycle_start_when_started_noop(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    uint32_t v_before = bb_lifecycle_version(svc);
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_start(svc));
    TEST_ASSERT_EQUAL_UINT32(v_before, bb_lifecycle_version(svc));
}

void test_bb_lifecycle_clear_never_interned_noop_no_bump(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    uint32_t v_before = bb_lifecycle_version(svc);
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_clear(svc, "never-seen-before"));
    TEST_ASSERT_EQUAL_UINT32(v_before, bb_lifecycle_version(svc));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_state(svc));
}

void test_bb_lifecycle_pause_clear_null_reason_invalid_arg(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_pause_clear(svc, NULL));
}

void test_bb_lifecycle_pause_clear_bad_handle_not_found(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_lifecycle_pause_clear((bb_lifecycle_svc_t)999, TEST_R_OTA));
}

// ---------------------------------------------------------------------------
// PUSH observer delivery
// ---------------------------------------------------------------------------

void test_bb_lifecycle_push_fires_observers_in_order_with_payload(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    int user_token = 42;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe(observer_a, &user_token));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe(observer_b, NULL));

    bb_lifecycle_start(svc);

    TEST_ASSERT_EQUAL(1, s_obs_calls); // only observer_a increments s_obs_calls
    TEST_ASSERT_EQUAL(2, s_obs_order_count);
    TEST_ASSERT_EQUAL(1, s_obs_order[0]); // observer_a fired first (registration order)
    TEST_ASSERT_EQUAL(2, s_obs_order[1]);
    TEST_ASSERT_EQUAL_PTR(&user_token, s_obs_last_user);
    TEST_ASSERT_EQUAL((int32_t)svc, s_obs_last.svc);
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, s_obs_last.old_state);
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, s_obs_last.new_state);
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_REASON_NONE, s_obs_last.reason);
}

void test_bb_lifecycle_push_reentrancy_no_deadlock(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe(observer_reentrant, (void *)(intptr_t)svc));

    bb_lifecycle_pause_assert(svc, TEST_R_OTA);
    TEST_ASSERT_EQUAL(1, s_obs_calls);
    TEST_ASSERT_TRUE(s_reentrant_result); // observer's own is_paused() query saw the committed state
}

void test_bb_lifecycle_observer_capacity_overflow_no_space(void)
{
    test_bb_lifecycle_reset_local();

    bb_err_t err = BB_OK;
    for (int i = 0; i < TEST_MAX_OBSERVERS; i++) {
        err = bb_lifecycle_observe(observer_b, NULL);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_lifecycle_observe(observer_a, NULL));
}

void test_bb_lifecycle_observe_null_cb_invalid_arg(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_observe(NULL, NULL));
}

// ---------------------------------------------------------------------------
// PULL (generic emit sink)
// ---------------------------------------------------------------------------

void test_bb_lifecycle_pull_null_sink_no_crash_still_bumps_and_fires(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe(observer_a, NULL));

    uint32_t v_before = bb_lifecycle_version(svc);
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_start(svc)); // no emit sink registered -- must not crash
    TEST_ASSERT_TRUE(bb_lifecycle_version(svc) > v_before);
    TEST_ASSERT_EQUAL(1, s_obs_calls); // push still fires independent of pull sink
}

void test_bb_lifecycle_pull_stub_sink_receives_topic_id_payload(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_set_emit(stub_emit, NULL);

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    TEST_ASSERT_EQUAL(1, s_emit_calls);
    TEST_ASSERT_EQUAL_STRING(BB_LIFECYCLE_EVENT_TOPIC, s_emit_topic);
    TEST_ASSERT_EQUAL((int32_t)svc, s_emit_id);
    TEST_ASSERT_EQUAL((int32_t)svc, s_emit_payload.svc);
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, s_emit_payload.new_state);
    // B1-1045 PR-1: bb_lifecycle_set_emit still takes only a bare cb (no
    // ctx param yet) -- bb_lifecycle_emit_invoke's internal cb(...) call
    // always passes a NULL ctx until a later PR wires a real per-consumer
    // binding.
    TEST_ASSERT_NULL(s_emit_ctx);

    bb_lifecycle_set_emit(NULL, NULL); // clean up before reset_for_test also clears it
}

// ---------------------------------------------------------------------------
// Emit-seam producer binding (B1-1045 PR-1) -- caller-owned, round-trip.
// ---------------------------------------------------------------------------

static bb_lifecycle_action_t classify_always_start(uint32_t id, const void *payload, size_t size)
{
    (void)id;
    (void)payload;
    (void)size;
    return BB_LIFECYCLE_ACTION_START;
}

static bb_lifecycle_action_t classify_always_stop(uint32_t id, const void *payload, size_t size)
{
    (void)id;
    (void)payload;
    (void)size;
    return BB_LIFECYCLE_ACTION_STOP;
}

static bb_lifecycle_action_t classify_always_none(uint32_t id, const void *payload, size_t size)
{
    (void)id;
    (void)payload;
    (void)size;
    return BB_LIFECYCLE_ACTION_NONE;
}

void test_bb_lifecycle_emit_binding_init_null_args_invalid(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-bind" };
    bb_lifecycle_svc_t svc;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc));

    bb_lifecycle_binding_t binding;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_emit_binding_init(NULL, svc, classify_always_start));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_emit_binding_init(&binding, svc, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_lifecycle_emit_binding_init(&binding, BB_LIFECYCLE_SVC_INVALID, classify_always_start));
}

// Round-trip: bb_lifecycle_emit_binding_fn()'s trampoline reads ctx as the
// bb_lifecycle_binding_t built by bb_lifecycle_emit_binding_init(), and
// drives the bound service's start/stop per the classify function's
// verdict -- exercised via the real bb_emit_fn call shape (ctx, topic, id,
// payload, size), not a mirror of the trampoline's internals.
void test_bb_lifecycle_emit_binding_trampoline_start_drives_service(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-bind" };
    bb_lifecycle_svc_t svc;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_state(svc));

    bb_lifecycle_binding_t binding;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_emit_binding_init(&binding, svc, classify_always_start));

    bb_emit_fn trampoline = bb_lifecycle_emit_binding_fn();
    TEST_ASSERT_NOT_NULL(trampoline);
    trampoline(&binding, "producer.topic", 1, NULL, 0);

    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_state(svc));
}

void test_bb_lifecycle_emit_binding_trampoline_stop_drives_service(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-bind" };
    bb_lifecycle_svc_t svc;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc));
    bb_lifecycle_start(svc);
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_state(svc));

    bb_lifecycle_binding_t binding;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_emit_binding_init(&binding, svc, classify_always_stop));

    bb_emit_fn trampoline = bb_lifecycle_emit_binding_fn();
    trampoline(&binding, "producer.topic", 2, NULL, 0);

    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_state(svc));
}

void test_bb_lifecycle_emit_binding_trampoline_none_is_noop(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-bind" };
    bb_lifecycle_svc_t svc;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_register(&cfg, &svc));
    uint32_t v_before = bb_lifecycle_version(svc);

    bb_lifecycle_binding_t binding;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_emit_binding_init(&binding, svc, classify_always_none));

    bb_emit_fn trampoline = bb_lifecycle_emit_binding_fn();
    trampoline(&binding, "producer.topic", 3, NULL, 0);

    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_state(svc));
    TEST_ASSERT_EQUAL(v_before, bb_lifecycle_version(svc));
}

// NULL/malformed ctx is a no-op, never a crash.
void test_bb_lifecycle_emit_binding_trampoline_null_ctx_is_noop(void)
{
    bb_emit_fn trampoline = bb_lifecycle_emit_binding_fn();
    TEST_ASSERT_NOT_NULL(trampoline);
    trampoline(NULL, "producer.topic", 1, NULL, 0); // must not crash
}

// ---------------------------------------------------------------------------
// POLL (version)
// ---------------------------------------------------------------------------

void test_bb_lifecycle_version_monotonic_on_real_transitions(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    uint32_t v0 = bb_lifecycle_version(svc);
    bb_lifecycle_start(svc);
    uint32_t v1 = bb_lifecycle_version(svc);
    TEST_ASSERT_TRUE(v1 > v0);

    bb_lifecycle_pause_assert(svc, TEST_R_OTA);
    uint32_t v2 = bb_lifecycle_version(svc);
    TEST_ASSERT_TRUE(v2 > v1);
}

void test_bb_lifecycle_version_unchanged_on_noop(void)
{
    test_bb_lifecycle_reset_local();

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    uint32_t v_before = bb_lifecycle_version(svc);
    bb_lifecycle_start(svc); // already started -- no-op
    TEST_ASSERT_EQUAL_UINT32(v_before, bb_lifecycle_version(svc));
}

// ---------------------------------------------------------------------------
// count()
// ---------------------------------------------------------------------------

void test_bb_lifecycle_count_reflects_registrations(void)
{
    test_bb_lifecycle_reset_local();

    TEST_ASSERT_EQUAL((size_t)0, bb_lifecycle_count());
    bb_lifecycle_config_t cfg_a = { .name = "svc-a" };
    bb_lifecycle_config_t cfg_b = { .name = "svc-b" };
    bb_lifecycle_svc_t a, b;
    bb_lifecycle_register(&cfg_a, &a);
    TEST_ASSERT_EQUAL((size_t)1, bb_lifecycle_count());
    bb_lifecycle_register(&cfg_b, &b);
    TEST_ASSERT_EQUAL((size_t)2, bb_lifecycle_count());
}

// ---------------------------------------------------------------------------
// Multi-word bit math (pure helpers, tested directly -- bits >= default
// MAX_REASONS covered without rebuilding CONFIG_BB_LIFECYCLE_MAX_REASONS)
// ---------------------------------------------------------------------------

void test_bb_lifecycle_inhibit_words_math(void)
{
    test_bb_lifecycle_reset_local();

    // (32+31)/32 == 1 at the shipped default.
    TEST_ASSERT_EQUAL(1, BB_LIFECYCLE_INHIBIT_WORDS);
}

void test_bb_lifecycle_word_set_test_clear_multi_word_bits(void)
{
    test_bb_lifecycle_reset_local();

    uint32_t words[3] = {0, 0, 0};

    bb_lifecycle_word_set_for_test(words, 3, 5);
    bb_lifecycle_word_set_for_test(words, 3, 31);
    bb_lifecycle_word_set_for_test(words, 3, 32);
    bb_lifecycle_word_set_for_test(words, 3, 33);
    bb_lifecycle_word_set_for_test(words, 3, 63);

    TEST_ASSERT_TRUE(bb_lifecycle_word_test_for_test(words, 3, 5));
    TEST_ASSERT_TRUE(bb_lifecycle_word_test_for_test(words, 3, 31));
    TEST_ASSERT_TRUE(bb_lifecycle_word_test_for_test(words, 3, 32));
    TEST_ASSERT_TRUE(bb_lifecycle_word_test_for_test(words, 3, 33));
    TEST_ASSERT_TRUE(bb_lifecycle_word_test_for_test(words, 3, 63));
    TEST_ASSERT_FALSE(bb_lifecycle_word_test_for_test(words, 3, 6));

    TEST_ASSERT_EQUAL_UINT32((1u << 5) | (1u << 31), words[0]);
    // word1 covers bits 32..63: bit32->offset0, bit33->offset1, bit63->offset31.
    TEST_ASSERT_EQUAL_UINT32((1u << 0) | (1u << 1) | (1u << 31), words[1]);
    TEST_ASSERT_EQUAL_UINT32(0u, words[2]);

    bb_lifecycle_word_clear_for_test(words, 3, 32);
    TEST_ASSERT_FALSE(bb_lifecycle_word_test_for_test(words, 3, 32));
    TEST_ASSERT_TRUE(bb_lifecycle_word_test_for_test(words, 3, 33)); // sibling bit untouched
}

void test_bb_lifecycle_word_ops_out_of_range_nwords_no_crash(void)
{
    test_bb_lifecycle_reset_local();

    uint32_t words[1] = {0};
    // bit 40 falls in word index 1, out of range for nwords=1 -- no-op, no OOB write.
    bb_lifecycle_word_set_for_test(words, 1, 40);
    TEST_ASSERT_EQUAL_UINT32(0, words[0]);
    TEST_ASSERT_FALSE(bb_lifecycle_word_test_for_test(words, 1, 40));
    bb_lifecycle_word_clear_for_test(words, 1, 40); // must not crash
}

void test_bb_lifecycle_compute_state_pure(void)
{
    test_bb_lifecycle_reset_local();

    uint32_t zero_words[1] = {0};
    uint32_t set_words[1]  = {1u};

    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_compute_state_for_test(false, zero_words, 1));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_STOPPED, bb_lifecycle_compute_state_for_test(false, set_words, 1)); // STOPPED dominates
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_compute_state_for_test(true, zero_words, 1));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_PAUSED, bb_lifecycle_compute_state_for_test(true, set_words, 1));
}

// ---------------------------------------------------------------------------
// Reason truncation foot-gun
// ---------------------------------------------------------------------------

void test_bb_lifecycle_reason_truncation_equal_prefix_maps_same_bit(void)
{
    test_bb_lifecycle_reset_local();

    // CONFIG_BB_LIFECYCLE_REASON_MAX default is 24 (23 usable chars + NUL).
    // Two strings differing only after the truncation point must intern to
    // the SAME bit.
    char long_a[64];
    char long_b[64];
    memset(long_a, 'x', sizeof(long_a) - 1);
    long_a[sizeof(long_a) - 1] = '\0';
    memcpy(long_b, long_a, sizeof(long_b));
    long_b[40] = 'Y'; // differs only well past the REASON_MAX truncation point

    bb_lifecycle_config_t cfg = { .name = "svc-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);
    bb_lifecycle_start(svc);

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, long_a));
    uint32_t words_a[BB_LIFECYCLE_INHIBIT_WORDS] = {0};
    bb_lifecycle_inhibit_words(svc, words_a, BB_LIFECYCLE_INHIBIT_WORDS);

    // clearing the truncated-equal long_b must clear the SAME bit long_a set
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_clear(svc, long_b));
    TEST_ASSERT_EQUAL(BB_LIFECYCLE_RUNNING, bb_lifecycle_state(svc));
}
