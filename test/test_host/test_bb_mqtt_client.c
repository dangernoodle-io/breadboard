// Tests for bb_mqtt_client host stub:
// - publish captures topic/payload/qos/retain
// - client_id default (hostname) / override / empty (null) resolution
// - is_connected flag settable via bb_mqtt_client_host_set_connected
// - B1-276: disable/teardown path classified separately from enable path
#include "unity.h"
#include "bb_mqtt_client.h"
#include "bb_nv.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_mqtt_client_t make_client(const char *uri, const char *client_id)
{
    bb_mqtt_client_cfg_t cfg = {
        .uri       = uri ? uri : "mqtt://localhost:1883",
        .client_id = client_id,
        .tls       = false,
    };
    bb_mqtt_client_t h = NULL;
    bb_err_t rc = bb_mqtt_client_init(&cfg, &h);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(h);
    return h;
}

// ---------------------------------------------------------------------------
// Publish capture tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_publish_captures_topic(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_publish(h, "test/topic", "hello", -1, 0, false);
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("test/topic", p->topic);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_captures_payload(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_publish(h, "t", "my-payload", -1, 0, false);
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("my-payload", p->payload);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_captures_qos_retain(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_publish(h, "t", "v", -1, 1, true);
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(1, p->qos);
    TEST_ASSERT_TRUE(p->retain);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_count_increments(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_pub_count(h));
    bb_mqtt_client_publish(h, "a", "1", -1, 0, false);
    bb_mqtt_client_publish(h, "b", "2", -1, 0, false);
    TEST_ASSERT_EQUAL_INT(2, bb_mqtt_client_host_pub_count(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_last_is_most_recent(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_publish(h, "first",  "1", -1, 0, false);
    bb_mqtt_client_publish(h, "second", "2", -1, 0, false);
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("second", p->topic);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_explicit_len(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_publish(h, "t", "hello world", 5, 0, false);
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    // len=5 → "hello"
    TEST_ASSERT_EQUAL_INT(0, strncmp("hello", p->payload, 5));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_host_reset_clears_pubs(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_publish(h, "t", "v", -1, 0, false);
    bb_mqtt_client_host_reset(h);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_pub_count(h));
    TEST_ASSERT_NULL(bb_mqtt_client_host_last_pub(h));
    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// client_id resolution tests (host stub does not enforce hostname lookup,
// but init must succeed for all three modes)
// ---------------------------------------------------------------------------

void test_bb_mqtt_init_client_id_null_uses_hostname(void)
{
    // NULL client_id → default (hostname); init must succeed.
    bb_mqtt_client_t h = make_client(NULL, NULL);  // client_id=NULL
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_init_client_id_override(void)
{
    // Non-empty string → override.
    bb_mqtt_client_t h = make_client(NULL, "my-device-01");
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_init_client_id_empty_broker_assigned(void)
{
    // Empty string → set_null_client_id (broker assigns ID); init must succeed.
    bb_mqtt_client_t h = make_client(NULL, "");
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// is_connected flag tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_is_connected_default_true(void)
{
    // Host stub starts connected=true so publish tests work without extra setup.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_TRUE(bb_mqtt_client_is_connected(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_is_connected_set_false(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_connected(h, false);
    TEST_ASSERT_FALSE(bb_mqtt_client_is_connected(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_is_connected_set_true(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_connected(h, false);
    bb_mqtt_client_host_set_connected(h, true);
    TEST_ASSERT_TRUE(bb_mqtt_client_is_connected(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_is_connected_null_returns_false(void)
{
    TEST_ASSERT_FALSE(bb_mqtt_client_is_connected(NULL));
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

void test_bb_mqtt_init_null_cfg_returns_invalid_arg(void)
{
    bb_mqtt_client_t h = NULL;
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_init(NULL, &h));
}

void test_bb_mqtt_init_null_out_returns_invalid_arg(void)
{
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost" };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_init(&cfg, NULL));
}

void test_bb_mqtt_destroy_null_is_safe(void)
{
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_destroy(NULL));
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_default / bb_mqtt_client_default_set tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_default_returns_null_initially(void)
{
    // Host stub starts with NULL default (no autoregister on host).
    // Reset to NULL first in case a prior test left it set.
    bb_mqtt_client_default_set(NULL);
    TEST_ASSERT_NULL(bb_mqtt_client_default());
}

void test_bb_mqtt_default_returns_set_handle(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_default_cleared_by_set_null(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_default_set(NULL);
    TEST_ASSERT_NULL(bb_mqtt_client_default());
    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_stop_default tests (B1-289)
// ---------------------------------------------------------------------------

void test_bb_mqtt_stop_default_null_default_is_safe(void)
{
    // stop_default with no default handle must not crash.
    bb_mqtt_client_default_set(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop_default());
    TEST_ASSERT_NULL(bb_mqtt_client_default());
}

void test_bb_mqtt_stop_default_clears_default(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop_default());
    // After stop_default the default must be NULL (host: stop NULLs via pointer).
    TEST_ASSERT_NULL(bb_mqtt_client_default());
}

void test_bb_mqtt_stop_default_idempotent(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop_default());  // second call safe
    TEST_ASSERT_NULL(bb_mqtt_client_default());
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_stop tests — lifecycle: disable→enabled guard, stop→NULL, idempotent
// ---------------------------------------------------------------------------

void test_bb_mqtt_stop_null_handle_p_is_safe(void)
{
    // bb_mqtt_client_stop(NULL) must not crash.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(NULL));
}

void test_bb_mqtt_stop_null_deref_is_safe(void)
{
    // bb_mqtt_client_stop(&h) where h==NULL must not crash.
    bb_mqtt_client_t h = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(&h));
    TEST_ASSERT_NULL(h);
}

void test_bb_mqtt_stop_clears_handle(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(&h));
    // After stop the handle must be NULL — caller cannot accidentally reuse it.
    TEST_ASSERT_NULL(h);
}

void test_bb_mqtt_stop_idempotent(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(&h));
    // Second call: handle is already NULL → must return BB_OK without crash.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(&h));
    TEST_ASSERT_NULL(h);
}

void test_bb_mqtt_stop_publish_after_stop_returns_error(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(&h));
    // After stop the handle is NULL; publish must return INVALID_ARG (not crash).
    bb_err_t rc = bb_mqtt_client_publish(h, "t", "v", -1, 0, false);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

// ---------------------------------------------------------------------------
// Lifecycle: disabled→enabled guard tests (host approximation)
// These exercise the guard logic paths that mirror the ESP-IDF reconfigure
// null-client guard (no client to stop/destroy when enabling from disabled).
// ---------------------------------------------------------------------------

void test_bb_mqtt_lifecycle_init_stop_reinit(void)
{
    // Simulates: init → stop → reinit (hot-reconnect path without real mqtt).
    // Verifies the handle pointer semantics: stop clears to NULL, reinit gives
    // a fresh handle, publish works on the new handle.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(&h));
    TEST_ASSERT_NULL(h);

    // Re-init (new handle).
    h = make_client(NULL, NULL);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_mqtt_client_publish(h, "test/topic", "payload", -1, 0, false));
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_pub_count(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_lifecycle_enabled_disabled_enabled(void)
{
    // enabled → disabled (stop) → enabled (reinit): each phase must be clean.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_publish(h, "t", "1", -1, 0, false);
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_pub_count(h));

    // Disable: stop frees and NULLs the handle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(&h));
    TEST_ASSERT_NULL(h);

    // Re-enable: fresh handle, clean pub count.
    h = make_client(NULL, NULL);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_pub_count(h));

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_default_null_after_stop(void)
{
    // Mirrors the ESP-IDF path where s_auto_client is NULLed after stop.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_stop(&h));
    // Caller is responsible for clearing the default after stop (mirrors
    // bb_mqtt_client_reconfigure setting s_auto_client = NULL).
    bb_mqtt_client_default_set(NULL);
    TEST_ASSERT_NULL(bb_mqtt_client_default());
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_suspend_default / bb_mqtt_client_resume_default tests
//
// Semantics under full-release/recreate model:
//  - suspend DESTROYS the handle: bb_mqtt_client_default() returns NULL, suspended=true.
//  - resume RECREATES: bb_mqtt_client_default() returns a new non-NULL handle,
//    suspended=false, connected=true (host stub default).
//  - The original handle value captured before suspend is invalid after
//    suspend — do NOT call bb_mqtt_client_destroy on it; the host stub already freed
//    it inside bb_mqtt_client_suspend_default.
// ---------------------------------------------------------------------------

void test_bb_mqtt_suspend_default_no_client_is_safe(void)
{
    // suspend with no default handle must not crash and must return BB_OK.
    bb_mqtt_client_default_set(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    // Clean up the suspended flag for subsequent tests.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
}

void test_bb_mqtt_resume_default_no_client_is_safe(void)
{
    // resume when not suspended must be a no-op and return BB_OK.
    bb_mqtt_client_default_set(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    // Clean up any recreated handle.
    bb_mqtt_client_stop_default();
}

void test_bb_mqtt_suspend_default_sets_suspended(void)
{
    // After suspend: suspended flag set, handle DESTROYED (NULL).
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_FALSE(bb_mqtt_client_host_is_suspended_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    // Handle was destroyed by suspend — bb_mqtt_client_default() must be NULL.
    TEST_ASSERT_NULL(bb_mqtt_client_default());
    // h is now a dangling pointer; do NOT call bb_mqtt_client_destroy(h).
    // Resume to restore consistent state for subsequent tests.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    bb_mqtt_client_stop_default();
}

void test_bb_mqtt_resume_default_clears_suspended(void)
{
    // After resume: suspended=false, default handle recreated + connected.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    // h is destroyed at this point.

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_client_host_is_suspended_default());
    // A fresh handle must exist and be connected.
    TEST_ASSERT_NOT_NULL(bb_mqtt_client_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_is_connected(bb_mqtt_client_default()));

    bb_mqtt_client_stop_default();
}

void test_bb_mqtt_suspend_default_idempotent(void)
{
    // double-suspend must be a no-op and return BB_OK both times.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());  // second: no-op
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    TEST_ASSERT_NULL(bb_mqtt_client_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    bb_mqtt_client_stop_default();
}

void test_bb_mqtt_resume_default_idempotent(void)
{
    // double-resume (second call is not suspended) must be a no-op + BB_OK.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    // h destroyed here.

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_client_host_is_suspended_default());
    TEST_ASSERT_NOT_NULL(bb_mqtt_client_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());  // second resume: no-op
    TEST_ASSERT_FALSE(bb_mqtt_client_host_is_suspended_default());
    // The handle from the first resume is still alive (not re-destroyed).
    TEST_ASSERT_NOT_NULL(bb_mqtt_client_default());

    bb_mqtt_client_stop_default();
}

void test_bb_mqtt_suspend_resume_cycle(void)
{
    // Full destroy → recreate cycle; verify the round-trip twice.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);

    // First cycle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    TEST_ASSERT_NULL(bb_mqtt_client_default());
    // h is now destroyed.

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_client_host_is_suspended_default());
    TEST_ASSERT_NOT_NULL(bb_mqtt_client_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_is_connected(bb_mqtt_client_default()));

    // Second cycle: suspend the recreated handle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    TEST_ASSERT_NULL(bb_mqtt_client_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_client_host_is_suspended_default());
    TEST_ASSERT_NOT_NULL(bb_mqtt_client_default());

    bb_mqtt_client_stop_default();
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_suspend_default / bb_mqtt_client_resume_default — stop-only path tests
//
// These tests set bb_mqtt_client_host_set_stop_only(true) to model the
// CONFIG_BB_MQTT_CLIENT_SUSPEND_STOP_ONLY=y ESP-IDF behavior:
//   - suspend keeps handle RESIDENT (s_default_handle NON-NULL, connected=false)
//   - resume reconnects on the SAME handle (connected=true, no recreate)
//   - reconnect_count is NOT incremented (no NVS reload / destroy)
//
// All tests restore stop_only=false at the end to avoid polluting other tests.
// ---------------------------------------------------------------------------

void test_bb_mqtt_stop_only_suspend_handle_survives(void)
{
    // After suspend in stop-only mode the default handle must NOT be destroyed.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_stop_only(true);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    // Handle must still be non-NULL (resident).
    TEST_ASSERT_NOT_NULL(bb_mqtt_client_default());
    // Must be the same pointer (not a new allocation).
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());
    // Connected must be false (stopped).
    TEST_ASSERT_FALSE(bb_mqtt_client_is_connected(bb_mqtt_client_default()));

    // Cleanup.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    bb_mqtt_client_stop_default();
    bb_mqtt_client_host_set_stop_only(false);
}

void test_bb_mqtt_stop_only_resume_reconnects_same_handle(void)
{
    // Resume in stop-only mode must reconnect on the SAME handle pointer.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_stop_only(true);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    bb_mqtt_client_t h_after_suspend = bb_mqtt_client_default();
    TEST_ASSERT_EQUAL_PTR(h, h_after_suspend);  // resident

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_client_host_is_suspended_default());
    // Must still be the original pointer (no recreate).
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());
    // Must be connected again.
    TEST_ASSERT_TRUE(bb_mqtt_client_is_connected(bb_mqtt_client_default()));

    bb_mqtt_client_stop_default();
    bb_mqtt_client_host_set_stop_only(false);
}

void test_bb_mqtt_stop_only_cycle_idempotent(void)
{
    // Two full stop-only suspend/resume cycles must both work cleanly.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_stop_only(true);

    // First cycle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());  // resident
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());  // same handle
    TEST_ASSERT_TRUE(bb_mqtt_client_is_connected(bb_mqtt_client_default()));

    // Second cycle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_is_connected(bb_mqtt_client_default()));

    bb_mqtt_client_stop_default();
    bb_mqtt_client_host_set_stop_only(false);
}

void test_bb_mqtt_stop_only_reconnect_count_unchanged(void)
{
    // Suspend/resume in stop-only mode must NOT increment reconnect_count
    // (no destroy/recreate, no simulated reconnect event).
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_stop_only(true);

    bb_mqtt_client_stats_t stats_before;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats_before));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());

    bb_mqtt_client_stats_t stats_after;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(bb_mqtt_client_default(), &stats_after));
    // reconnect_count must not have changed (no NVS reload, no new handle).
    TEST_ASSERT_EQUAL_UINT32(stats_before.reconnect_count, stats_after.reconnect_count);

    bb_mqtt_client_stop_default();
    bb_mqtt_client_host_set_stop_only(false);
}

void test_bb_mqtt_stop_only_suspend_idempotent(void)
{
    // double-suspend in stop-only mode must be a no-op and return BB_OK.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_stop_only(true);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());  // no-op
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_client_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    bb_mqtt_client_stop_default();
    bb_mqtt_client_host_set_stop_only(false);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_publish ring overflow tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_publish_ring_overflow_evicts_oldest(void)
{
    // Push BB_MQTT_CLIENT_HOST_PUB_CAP (32) + 1 publishes; the ring must shift out the
    // oldest entry and the last captured pub must be the final one.
    bb_mqtt_client_t h = make_client(NULL, NULL);

    char topic[32];
    char payload[32];
    for (int i = 0; i < 33; i++) {
        snprintf(topic,   sizeof(topic),   "t/%d", i);
        snprintf(payload, sizeof(payload), "p%d",  i);
        TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_publish(h, topic, payload, -1, 0, false));
    }

    // Ring is capped at 32; count must not exceed 32.
    TEST_ASSERT_EQUAL_INT(32, bb_mqtt_client_host_pub_count(h));

    // Last pub must be the 33rd message (index 32).
    const bb_mqtt_client_host_pub_t *last = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("t/32",  last->topic);
    TEST_ASSERT_EQUAL_STRING("p32",   last->payload);

    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_host_simulate_reconnect tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_simulate_reconnect_increments_count(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);

    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, stats.reconnect_count);

    bb_mqtt_client_host_simulate_reconnect(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, stats.reconnect_count);

    bb_mqtt_client_host_simulate_reconnect(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_UINT32(2, stats.reconnect_count);

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_simulate_reconnect_null_handle_is_safe(void)
{
    // Must not crash on NULL handle.
    bb_mqtt_client_host_simulate_reconnect(NULL);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_get_stats new fields (B1-362): disc_reason, tls_fail, tls_error_code
// ---------------------------------------------------------------------------

void test_bb_mqtt_disc_reason_default_is_none(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_INT(BB_MQTT_CLIENT_DISC_NONE, stats.disc_reason);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_disc_reason_round_trips(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_disc_reason(h, BB_MQTT_CLIENT_DISC_TRANSPORT);
    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_INT(BB_MQTT_CLIENT_DISC_TRANSPORT, stats.disc_reason);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_disc_reason_null_handle_is_safe(void)
{
    // must not crash
    bb_mqtt_client_host_set_disc_reason(NULL, BB_MQTT_CLIENT_DISC_TRANSPORT);
}

void test_bb_mqtt_tls_fail_default_is_none(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_INT(BB_TLS_FAIL_NONE, stats.tls_fail);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_tls_fail_round_trips(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_tls_fail(h, BB_TLS_FAIL_RECORD_TOO_BIG);
    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_INT(BB_TLS_FAIL_RECORD_TOO_BIG, stats.tls_fail);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_tls_fail_null_handle_is_safe(void)
{
    bb_mqtt_client_host_set_tls_fail(NULL, BB_TLS_FAIL_RECORD_TOO_BIG);
}

void test_bb_mqtt_tls_error_code_default_is_zero(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_INT(0, stats.tls_error_code);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_tls_error_code_round_trips(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_tls_error_code(h, -0x7200);
    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_INT(-0x7200, stats.tls_error_code);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_tls_error_code_null_handle_is_safe(void)
{
    bb_mqtt_client_host_set_tls_error_code(NULL, -0x7200);
}

void test_bb_mqtt_host_reset_clears_new_fields(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_disc_reason(h, BB_MQTT_CLIENT_DISC_TRANSPORT);
    bb_mqtt_client_host_set_tls_fail(h, BB_TLS_FAIL_RECORD_TOO_BIG);
    bb_mqtt_client_host_set_tls_error_code(h, -0x7200);
    bb_mqtt_client_host_reset(h);
    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_INT(BB_MQTT_CLIENT_DISC_NONE, stats.disc_reason);
    TEST_ASSERT_EQUAL_INT(BB_TLS_FAIL_NONE, stats.tls_fail);
    TEST_ASSERT_EQUAL_INT(0, stats.tls_error_code);
    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_resume_default alloc-failure path
// ---------------------------------------------------------------------------

static void *failing_calloc(size_t n, size_t sz)
{
    (void)n; (void)sz;
    return NULL;
}

void test_bb_mqtt_resume_default_init_failure_preserves_suspended(void)
{
    // When bb_mqtt_client_init fails inside resume_default (full-release mode),
    // the suspended flag must remain set so the caller can retry.
    bb_mqtt_client_default_set(NULL);                  // no prior client
    bb_mqtt_client_host_set_stop_only(false);          // full-release mode

    // Suspend with no handle — just sets the suspended flag.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());

    // Make the next calloc inside bb_mqtt_client_init fail.
    bb_mqtt_client_set_calloc(failing_calloc);
    bb_err_t rc = bb_mqtt_client_resume_default();
    bb_mqtt_client_set_calloc(NULL);   // restore before any assertion that might alloc

    // resume must propagate the init error.
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    // Suspended flag must still be set — caller can retry.
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    TEST_ASSERT_NULL(bb_mqtt_client_default());

    // Clean up: restore suspended state and clear the recreated default handle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    bb_mqtt_client_stop_default();
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_subscribe tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_subscribe_null_handle_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_subscribe(NULL, "t/#", 0));
}

void test_bb_mqtt_subscribe_null_topic_returns_invalid_arg(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_subscribe(h, NULL, 0));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_subscribe_happy_path_returns_ok(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_subscribe(h, "test/#", 0));
    bb_mqtt_client_destroy(h);
}

// bb_mqtt_client_host_set_subscribe_fail (B1-487): lets a later consumer (e.g. an MQTT
// ingress adapter) cover its subscribe-failure branch without a real broker.
void test_bb_mqtt_host_set_subscribe_fail_forces_error(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_subscribe_fail(h, true);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, bb_mqtt_client_subscribe(h, "test/#", 0));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_host_set_subscribe_fail_can_be_cleared(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_subscribe_fail(h, true);
    bb_mqtt_client_host_set_subscribe_fail(h, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_subscribe(h, "test/#", 0));
    bb_mqtt_client_destroy(h);
}


