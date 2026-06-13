// Tests for bb_mqtt host stub:
// - publish captures topic/payload/qos/retain
// - client_id default (hostname) / override / empty (null) resolution
// - is_connected flag settable via bb_mqtt_host_set_connected
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
// bb_mqtt_reconfigure tests (host stub)
// ---------------------------------------------------------------------------

void test_bb_mqtt_reconfigure_increments_count(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);  // resets s_reconfigure_count to 0
    int before = bb_mqtt_test_reconfigure_count();
    bb_err_t rc = bb_mqtt_reconfigure();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(before + 1, bb_mqtt_test_reconfigure_count());
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_reconfigure_idempotent(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);
    bb_mqtt_reconfigure();
    bb_mqtt_reconfigure();
    bb_mqtt_reconfigure();
    TEST_ASSERT_EQUAL_INT(3, bb_mqtt_test_reconfigure_count());
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_host_reset_clears_reconfigure_count(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);
    bb_mqtt_reconfigure();
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_test_reconfigure_count());
    bb_mqtt_host_reset(h);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_test_reconfigure_count());
    bb_mqtt_destroy(h);
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

void test_bb_mqtt_reconfigure_from_no_client_is_safe(void)
{
    // Simulates: MQTT disabled at boot (s_auto_client == NULL), PATCH enables.
    // On the host stub bb_mqtt_reconfigure is a counter increment; it must
    // return BB_OK without crashing even when called with no prior client.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);
    bb_err_t rc = bb_mqtt_reconfigure();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    bb_mqtt_destroy(h);
}

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
// Deferred reconfigure path tests (host approximation of the ESP-IDF one-shot
// worker task).  On the host the worker runs synchronously inside
// bb_mqtt_reconfigure; these tests assert the observable lifecycle bookkeeping
// that is identical across both backends.
// ---------------------------------------------------------------------------

void test_bb_mqtt_reconfigure_deferred_triggers_lifecycle(void)
{
    // Calling bb_mqtt_reconfigure() must trigger exactly one lifecycle cycle
    // (increment the counter) and return BB_OK — this mirrors the ESP-IDF
    // path where the one-shot task does the heavy work then self-deletes.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);
    int before = bb_mqtt_test_reconfigure_count();
    bb_err_t rc = bb_mqtt_reconfigure();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(before + 1, bb_mqtt_test_reconfigure_count());
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_reconfigure_reentrancy_coalesces(void)
{
    // Rapid successive calls must all return BB_OK.  On ESP-IDF only one
    // worker task spawns at a time (lock held); on the host stub each call
    // is synchronous so the counter increments, but the key invariant is
    // no error return.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_reconfigure());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_reconfigure());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_reconfigure());
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_reconfigure_enable_async_connect(void)
{
    // Simulate: MQTT disabled, PATCH sets enabled=1, bb_mqtt_reconfigure
    // is called.  On host the stub returns BB_OK immediately; connected state
    // would flip asynchronously on ESP-IDF once the broker handshake
    // completes.  Assert: reconfigure returns BB_OK (caller gets 204) and the
    // reconfigure count advances (lifecycle was triggered).
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);
    bb_mqtt_host_set_connected(h, false);  // starts disconnected
    bb_err_t rc = bb_mqtt_reconfigure();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    // One lifecycle cycle triggered regardless of connected flag.
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_test_reconfigure_count());
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_reconfigure_disable_stops_client(void)
{
    // Simulate: MQTT enabled, PATCH sets enabled=0.  The caller:
    //   1. Writes enabled=0 to NVS.
    //   2. Calls bb_mqtt_stop(&h) to tear down immediately.
    //   3. Calls bb_mqtt_reconfigure() so the worker re-reads NVS and skips
    //      re-init (enabled=0 path).
    // Assert: handle is NULL after stop; reconfigure still returns BB_OK.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    TEST_ASSERT_NULL(h);
    // Reconfigure with no handle (mirrors ESP-IDF disabled path).
    bb_err_t rc = bb_mqtt_reconfigure();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    bb_mqtt_destroy(h);  // h==NULL, safe
}

void test_bb_mqtt_reconfigure_reenable_after_disable(void)
{
    // Full cycle: enable → disable → re-enable.
    // Phase 1 — enabled.
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_reset(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_reconfigure());  // cycle 1
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_test_reconfigure_count());

    // Phase 2 — disable.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_stop(&h));
    TEST_ASSERT_NULL(h);

    // Phase 3 — re-enable: fresh handle, publish works.
    h = make_client(NULL, NULL);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_mqtt_publish(h, "t/after-reenable", "ok", -1, 0, false));
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_host_pub_count(h));

    bb_mqtt_destroy(h);
}
