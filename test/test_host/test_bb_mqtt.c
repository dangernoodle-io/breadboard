// Tests for bb_mqtt host stub:
// - publish captures topic/payload/qos/retain
// - client_id default (hostname) / override / empty (null) resolution
// - is_connected flag settable via bb_mqtt_host_set_connected
// - B1-276: disable/teardown path classified separately from enable path
#include "unity.h"
#include "bb_mqtt.h"
#include "bb_nv.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_mqtt_t make_client(const char *uri, const char *client_id)
{
    bb_mqtt_cfg_t cfg = {
        .uri       = uri ? uri : "mqtt://localhost:1883",
        .client_id = client_id,
        .tls       = false,
    };
    bb_mqtt_t h = NULL;
    bb_err_t rc = bb_mqtt_init(&cfg, &h);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(h);
    return h;
}

// ---------------------------------------------------------------------------
// Publish capture tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_publish_captures_topic(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "test/topic", "hello", -1, 0, false);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("test/topic", p->topic);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_captures_payload(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "my-payload", -1, 0, false);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("my-payload", p->payload);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_captures_qos_retain(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "v", -1, 1, true);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(1, p->qos);
    TEST_ASSERT_TRUE(p->retain);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_count_increments(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_host_pub_count(h));
    bb_mqtt_publish(h, "a", "1", -1, 0, false);
    bb_mqtt_publish(h, "b", "2", -1, 0, false);
    TEST_ASSERT_EQUAL_INT(2, bb_mqtt_host_pub_count(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_last_is_most_recent(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "first",  "1", -1, 0, false);
    bb_mqtt_publish(h, "second", "2", -1, 0, false);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("second", p->topic);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_explicit_len(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "hello world", 5, 0, false);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    // len=5 → "hello"
    TEST_ASSERT_EQUAL_INT(0, strncmp("hello", p->payload, 5));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_host_reset_clears_pubs(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "v", -1, 0, false);
    bb_mqtt_host_reset(h);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_host_pub_count(h));
    TEST_ASSERT_NULL(bb_mqtt_host_last_pub(h));
    bb_mqtt_destroy(h);
}

// ---------------------------------------------------------------------------
// client_id resolution tests (host stub does not enforce hostname lookup,
// but init must succeed for all three modes)
// ---------------------------------------------------------------------------

void test_bb_mqtt_init_client_id_null_uses_hostname(void)
{
    // NULL client_id → default (hostname); init must succeed.
    bb_mqtt_t h = make_client(NULL, NULL);  // client_id=NULL
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_init_client_id_override(void)
{
    // Non-empty string → override.
    bb_mqtt_t h = make_client(NULL, "my-device-01");
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_init_client_id_empty_broker_assigned(void)
{
    // Empty string → set_null_client_id (broker assigns ID); init must succeed.
    bb_mqtt_t h = make_client(NULL, "");
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_destroy(h);
}

// ---------------------------------------------------------------------------
// is_connected flag tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_is_connected_default_true(void)
{
    // Host stub starts connected=true so publish tests work without extra setup.
    bb_mqtt_t h = make_client(NULL, NULL);
    TEST_ASSERT_TRUE(bb_mqtt_is_connected(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_is_connected_set_false(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_set_connected(h, false);
    TEST_ASSERT_FALSE(bb_mqtt_is_connected(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_is_connected_set_true(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_set_connected(h, false);
    bb_mqtt_host_set_connected(h, true);
    TEST_ASSERT_TRUE(bb_mqtt_is_connected(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_is_connected_null_returns_false(void)
{
    TEST_ASSERT_FALSE(bb_mqtt_is_connected(NULL));
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

void test_bb_mqtt_init_null_cfg_returns_invalid_arg(void)
{
    bb_mqtt_t h = NULL;
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_init(NULL, &h));
}

void test_bb_mqtt_init_null_out_returns_invalid_arg(void)
{
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost" };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_init(&cfg, NULL));
}

void test_bb_mqtt_destroy_null_is_safe(void)
{
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_destroy(NULL));
}

// ---------------------------------------------------------------------------
// bb_mqtt_default / bb_mqtt_default_set tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_default_returns_null_initially(void)
{
    // Host stub starts with NULL default (no autoregister on host).
    // Reset to NULL first in case a prior test left it set.
    bb_mqtt_default_set(NULL);
    TEST_ASSERT_NULL(bb_mqtt_default());
}

void test_bb_mqtt_default_returns_set_handle(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_default());
    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_default_cleared_by_set_null(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    bb_mqtt_default_set(NULL);
    TEST_ASSERT_NULL(bb_mqtt_default());
    bb_mqtt_destroy(h);
}

// ---------------------------------------------------------------------------
// bb_mqtt_stop_default tests (B1-289)
// ---------------------------------------------------------------------------

void test_bb_mqtt_stop_default_null_default_is_safe(void)
{
    // stop_default with no default handle must not crash.
    bb_mqtt_default_set(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop_default());
    TEST_ASSERT_NULL(bb_mqtt_default());
}

void test_bb_mqtt_stop_default_clears_default(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop_default());
    // After stop_default the default must be NULL (host: stop NULLs via pointer).
    TEST_ASSERT_NULL(bb_mqtt_default());
}

void test_bb_mqtt_stop_default_idempotent(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop_default());  // second call safe
    TEST_ASSERT_NULL(bb_mqtt_default());
}

// ---------------------------------------------------------------------------
// bb_mqtt_stop tests — lifecycle: disable→enabled guard, stop→NULL, idempotent
// ---------------------------------------------------------------------------

void test_bb_mqtt_stop_null_handle_p_is_safe(void)
{
    // bb_mqtt_stop(NULL) must not crash.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(NULL));
}

void test_bb_mqtt_stop_null_deref_is_safe(void)
{
    // bb_mqtt_stop(&h) where h==NULL must not crash.
    bb_mqtt_t h = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    TEST_ASSERT_NULL(h);
}

void test_bb_mqtt_stop_clears_handle(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    // After stop the handle must be NULL — caller cannot accidentally reuse it.
    TEST_ASSERT_NULL(h);
}

void test_bb_mqtt_stop_idempotent(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    // Second call: handle is already NULL → must return BB_OK without crash.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    TEST_ASSERT_NULL(h);
}

void test_bb_mqtt_stop_publish_after_stop_returns_error(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    // After stop the handle is NULL; publish must return INVALID_ARG (not crash).
    bb_err_t rc = bb_mqtt_publish(h, "t", "v", -1, 0, false);
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
    bb_mqtt_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    TEST_ASSERT_NULL(h);

    // Re-init (new handle).
    h = make_client(NULL, NULL);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_mqtt_publish(h, "test/topic", "payload", -1, 0, false));
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_host_pub_count(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_lifecycle_enabled_disabled_enabled(void)
{
    // enabled → disabled (stop) → enabled (reinit): each phase must be clean.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "1", -1, 0, false);
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_host_pub_count(h));

    // Disable: stop frees and NULLs the handle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    TEST_ASSERT_NULL(h);

    // Re-enable: fresh handle, clean pub count.
    h = make_client(NULL, NULL);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_host_pub_count(h));

    bb_mqtt_destroy(h);
}

void test_bb_mqtt_default_null_after_stop(void)
{
    // Mirrors the ESP-IDF path where s_auto_client is NULLed after stop.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    // Caller is responsible for clearing the default after stop (mirrors
    // bb_mqtt_reconfigure setting s_auto_client = NULL).
    bb_mqtt_default_set(NULL);
    TEST_ASSERT_NULL(bb_mqtt_default());
}

// ---------------------------------------------------------------------------
// bb_mqtt_suspend_default / bb_mqtt_resume_default tests
//
// Semantics under full-release/recreate model:
//  - suspend DESTROYS the handle: bb_mqtt_default() returns NULL, suspended=true.
//  - resume RECREATES: bb_mqtt_default() returns a new non-NULL handle,
//    suspended=false, connected=true (host stub default).
//  - The original handle value captured before suspend is invalid after
//    suspend — do NOT call bb_mqtt_destroy on it; the host stub already freed
//    it inside bb_mqtt_suspend_default.
// ---------------------------------------------------------------------------

void test_bb_mqtt_suspend_default_no_client_is_safe(void)
{
    // suspend with no default handle must not crash and must return BB_OK.
    bb_mqtt_default_set(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_suspend_default());
    // Clean up the suspended flag for subsequent tests.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());
}

void test_bb_mqtt_resume_default_no_client_is_safe(void)
{
    // resume when not suspended must be a no-op and return BB_OK.
    bb_mqtt_default_set(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());
    // Clean up any recreated handle.
    bb_mqtt_stop_default();
}

void test_bb_mqtt_suspend_default_sets_suspended(void)
{
    // After suspend: suspended flag set, handle DESTROYED (NULL).
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_FALSE(bb_mqtt_host_is_suspended_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_host_is_suspended_default());
    // Handle was destroyed by suspend — bb_mqtt_default() must be NULL.
    TEST_ASSERT_NULL(bb_mqtt_default());
    // h is now a dangling pointer; do NOT call bb_mqtt_destroy(h).
    // Resume to restore consistent state for subsequent tests.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());
    bb_mqtt_stop_default();
}

void test_bb_mqtt_resume_default_clears_suspended(void)
{
    // After resume: suspended=false, default handle recreated + connected.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_host_is_suspended_default());
    // h is destroyed at this point.

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_host_is_suspended_default());
    // A fresh handle must exist and be connected.
    TEST_ASSERT_NOT_NULL(bb_mqtt_default());
    TEST_ASSERT_TRUE(bb_mqtt_is_connected(bb_mqtt_default()));

    bb_mqtt_stop_default();
}

void test_bb_mqtt_suspend_default_idempotent(void)
{
    // double-suspend must be a no-op and return BB_OK both times.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_suspend_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_suspend_default());  // second: no-op
    TEST_ASSERT_TRUE(bb_mqtt_host_is_suspended_default());
    TEST_ASSERT_NULL(bb_mqtt_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());
    bb_mqtt_stop_default();
}

void test_bb_mqtt_resume_default_idempotent(void)
{
    // double-resume (second call is not suspended) must be a no-op + BB_OK.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_suspend_default());
    // h destroyed here.

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_host_is_suspended_default());
    TEST_ASSERT_NOT_NULL(bb_mqtt_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());  // second resume: no-op
    TEST_ASSERT_FALSE(bb_mqtt_host_is_suspended_default());
    // The handle from the first resume is still alive (not re-destroyed).
    TEST_ASSERT_NOT_NULL(bb_mqtt_default());

    bb_mqtt_stop_default();
}

void test_bb_mqtt_suspend_resume_cycle(void)
{
    // Full destroy → recreate cycle; verify the round-trip twice.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);

    // First cycle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_host_is_suspended_default());
    TEST_ASSERT_NULL(bb_mqtt_default());
    // h is now destroyed.

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_host_is_suspended_default());
    TEST_ASSERT_NOT_NULL(bb_mqtt_default());
    TEST_ASSERT_TRUE(bb_mqtt_is_connected(bb_mqtt_default()));

    // Second cycle: suspend the recreated handle.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_host_is_suspended_default());
    TEST_ASSERT_NULL(bb_mqtt_default());

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_host_is_suspended_default());
    TEST_ASSERT_NOT_NULL(bb_mqtt_default());

    bb_mqtt_stop_default();
}

