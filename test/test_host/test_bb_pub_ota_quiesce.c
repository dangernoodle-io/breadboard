// Tests for bb_pub_ota_quiesce:
//  - Init before bb_update_check_init returns BB_ERR_INVALID_STATE.
//  - After init, bb_ota hooks are set (bb_ota_has_pause_hook returns true).
//  - Invoking the wired pause hook pauses bb_pub (bb_pub_is_paused() == true).
//  - Invoking the wired resume hook clears the pause.
//  - Update-check hooks fire during a run (bb_pub not paused after run).
//  - bb_pub_tick_once is a no-op while paused.
#include "unity.h"
#include "bb_pub_ota_quiesce.h"
#include "bb_pub.h"
#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_ota_hooks.h"
#include "bb_event.h"
#include "bb_http_client_host.h"
#include "bb_nv.h"
#include "bb_json.h"

#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void reset_world(void)
{
    // bb_update_check_init calls bb_event_topic_register, which requires the
    // event subsystem to be initialized (setUp resets it to uninit).
    bb_event_init(NULL);
    bb_update_check_reset_for_test();
    bb_ota_set_hooks(NULL, NULL);
    bb_pub_test_reset();
    bb_http_client_clear_mock();
}

// Minimal valid GitHub releases JSON body.
#define VALID_BODY \
    "{\"tag_name\":\"v1.0.0\",\"assets\":[{\"name\":\"firmware.bin\"," \
    "\"browser_download_url\":\"http://example.com/firmware.bin\"}]}"

// ---------------------------------------------------------------------------
// Counting sink used by the tick-noop test.
// ---------------------------------------------------------------------------

static int s_publish_calls;

static bb_err_t counting_pub(void *ctx, const char *topic,
                             const char *payload, int len)
{
    (void)ctx; (void)topic; (void)payload; (void)len;
    s_publish_calls++;
    return BB_OK;
}

// Always-publish sample function.
static bool always_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "x", 1.0);
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_ota_quiesce_init_before_update_check_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pub_ota_quiesce_init());
}

void test_bb_pub_ota_quiesce_ota_hook_set_after_init(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_ota_quiesce_init());
    TEST_ASSERT_TRUE(bb_ota_has_pause_hook());
}

void test_bb_pub_ota_quiesce_pause_hook_pauses_bb_pub(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_ota_quiesce_init());

    TEST_ASSERT_FALSE(bb_pub_is_paused());
    bool ok = bb_ota_pause();
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(bb_pub_is_paused());
}

void test_bb_pub_ota_quiesce_resume_hook_clears_pause(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_ota_quiesce_init());

    bb_ota_pause();
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    bb_ota_resume();
    TEST_ASSERT_FALSE(bb_pub_is_paused());
}

void test_bb_pub_ota_quiesce_update_check_hooks_fire_during_run(void)
{
    // After a successful update-check run, resume must have been called and
    // bb_pub must NOT be paused.
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_ota_quiesce_init());

    bb_update_check_set_releases_url("http://example.com/releases");
    bb_update_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    TEST_ASSERT_FALSE(bb_pub_is_paused());
}

void test_bb_pub_ota_quiesce_tick_is_noop_while_paused(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_ota_quiesce_init());

    bb_pub_sink_t sink = { .publish = counting_pub };
    bb_pub_set_sink(&sink);
    bb_pub_register_source("q_test", always_sample, NULL);

    s_publish_calls = 0;

    // Paused — tick must not publish.
    bb_ota_pause();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_tick_once());
    TEST_ASSERT_EQUAL(0, s_publish_calls);

    // Resumed — tick publishes.
    bb_ota_resume();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_tick_once());
    TEST_ASSERT_EQUAL(1, s_publish_calls);
}
