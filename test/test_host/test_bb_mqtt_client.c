// Tests for bb_mqtt_client host stub:
// - publish captures topic/payload/qos/retain
// - client_id default (hostname) / override / empty (null) resolution
// - is_connected flag settable via bb_mqtt_client_host_set_connected
// - B1-276: disable/teardown path classified separately from enable path
//
// B1-756 (bb_nv dissolution epic B1-708): resume_default's NVS "uri" reload
// now round-trips through bb_config (backend="nvs") instead of bb_nv's
// generic KV forwarder -- reset_world() registers fake_nvs_backend.h's fake
// "nvs" backend so the reload test below exercises a real backend instead
// of a no-op (the real "nvs" bb_storage backend is ESP-IDF-only).
#include "unity.h"
#include "bb_mqtt_client.h"
#include "bb_config.h"
#include "bb_mqtt_client_nvs.h"
#include "bb_serialize.h"
#include "bb_storage.h"
#include "fake_nvs_backend.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// Test-local field descriptor targeting the EXACT SAME address (backend/ns/
// key/type) as bb_mqtt_client_espidf.c's / bb_mqtt_client.c's (host)
// s_mqtt_uri_field -- a literal-address BITE test proving
// bb_mqtt_client_resume_default's NVS reload actually resolves the
// persisted "uri" value, not merely that it falls back to the hardcoded
// default.
static const bb_config_field_t s_test_mqtt_uri_field = {
    .id   = "test.mqtt.uri", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_mqtt", .key = "uri" },
    .max_len = 128,
};

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
// bb_mqtt_client_is_tls tests
//
// B1-...: is_tls lost its only direct test when the bb_pub/bb_sink_* cluster
// was deleted (it was only ever covered transitively). It still has live
// ESP-IDF callers (TLS-branch decisions in the reconfigure path) — direct
// tests restore real coverage.
// ---------------------------------------------------------------------------

void test_bb_mqtt_is_tls_true_when_cfg_tls_true(void)
{
    bb_mqtt_client_cfg_t cfg = {
        .uri       = "mqtts://localhost:8883",
        .client_id = NULL,
        .tls       = true,
    };
    bb_mqtt_client_t h = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_init(&cfg, &h));
    TEST_ASSERT_TRUE(bb_mqtt_client_is_tls(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_is_tls_false_when_cfg_tls_false(void)
{
    // make_client() builds a cfg with .tls = false.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_FALSE(bb_mqtt_client_is_tls(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_is_tls_null_handle_returns_false(void)
{
    TEST_ASSERT_FALSE(bb_mqtt_client_is_tls(NULL));
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
// bb_mqtt_client_enqueue tests (B1-834 PR-1)
//
// Mapping under test (bb_mqtt_client.h doc comment):
//   msg_id >= 0            -> BB_OK
//   msg_id == -2 (outbox)  -> BB_ERR_NO_SPACE
//   msg_id == -1 (generic) -> BB_ERR_VALIDATION
//   handle destroyed / client not present (mqtt_acquire_client on the espidf
//   backend) -> BB_ERR_INVALID_STATE. NOTE (B1-834 review HIGH-1): there is
//   NO "not connected" precondition -- verified against the pinned esp-mqtt
//   source (mqtt_client.c ~2263-2302), esp_mqtt_client_enqueue() has no
//   connection-state check at all; queueing while disconnected (then
//   draining on reconnect) is this API's whole documented purpose.
// ---------------------------------------------------------------------------

void test_bb_mqtt_enqueue_null_handle_returns_invalid_arg(void)
{
    // Mutation check: drop the `!handle` half of the guard -> this call
    // would instead crash dereferencing a NULL handle. TEST_ASSERT catches
    // a wrong-but-non-crashing return; the crash itself would fail the run.
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_enqueue(NULL, "t", "v", -1, 0, false));
}

void test_bb_mqtt_enqueue_null_topic_returns_invalid_arg(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    // Mutation check: drop the `!topic` half of the guard -> falls through
    // to bb_strlcpy(p->topic, NULL, ...) and crashes instead of returning
    // BB_ERR_INVALID_ARG.
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_enqueue(h, NULL, "v", -1, 0, false));
    bb_mqtt_client_destroy(h);
}

// B1-834 review HIGH-1: enqueue-while-disconnected must ACCEPT and queue
// the message (drained on reconnect), not reject -- the pinned esp-mqtt
// source (mqtt_client.c ~2263-2302) has no connection-state check in
// esp_mqtt_client_enqueue(). This replaces a prior (incorrect) test that
// asserted the opposite by reusing the general `connected` flag as a
// stand-in for the espidf backend's actual "handle destroyed" guard.
void test_bb_mqtt_enqueue_while_disconnected_still_accepted(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_connected(h, false);
    // Mutation check: reintroduce a `!h->connected` early-return -> this
    // assertion flips to BB_ERR_INVALID_STATE and fails.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "v", -1, 0, false));
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_enqueue_count(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_accepted_returns_ok_and_records_pub(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    // Default forced msg_id is 0 (accepted) — no hook call needed.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "test/topic", "hello", -1, 0, false));
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    // Mutation check: skip the record-on-accept block -> p becomes NULL and
    // the topic assertion below fails to compile-time-safe NULL deref guard
    // (TEST_ASSERT_NOT_NULL catches it first).
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("test/topic", p->topic);
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_enqueue_count(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_outbox_full_returns_no_space(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_enqueue_msg_id(h, -2);
    // Mutation check: swap the -2/-1 branch bodies -> this returns
    // BB_ERR_VALIDATION instead and the assertion fails.
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_enqueue(h, "t", "v", -1, 0, false));
    // A rejected enqueue must not be recorded as accepted.
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_enqueue_count(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_generic_reject_returns_validation(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_enqueue_msg_id(h, -1);
    // Mutation check: change `msg_id == -1` to `msg_id < 0` before the -2
    // check (reordering the branches) -> -1 would be caught by a combined
    // `< 0` first and misreport BB_ERR_NO_SPACE instead of BB_ERR_VALIDATION.
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION, bb_mqtt_client_enqueue(h, "t", "v", -1, 0, false));
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_enqueue_count(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_msg_id_hook_is_sticky(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_enqueue_msg_id(h, -2);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_enqueue(h, "t", "v", -1, 0, false));
    // Mutation check: reset test_enqueue_msg_id to 0 after one use (a
    // one-shot instead of sticky hook) -> the second call below would
    // unexpectedly return BB_OK.
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_enqueue(h, "t2", "v2", -1, 0, false));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_count_null_handle_returns_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_enqueue_count(NULL));
}

void test_bb_mqtt_enqueue_set_msg_id_null_handle_is_safe(void)
{
    // Must not crash on NULL handle.
    bb_mqtt_client_host_set_enqueue_msg_id(NULL, -1);
}

void test_bb_mqtt_enqueue_ring_overflow_evicts_oldest(void)
{
    // Mirrors test_bb_mqtt_publish_ring_overflow_evicts_oldest -- proves
    // bb_mqtt_client_enqueue()'s accepted-path ring bookkeeping (shared with
    // publish) also evicts the oldest entry once the ring is full.
    bb_mqtt_client_t h = make_client(NULL, NULL);

    char topic[32];
    char payload[32];
    for (int i = 0; i < 33; i++) {
        snprintf(topic,   sizeof(topic),   "t/%d", i);
        snprintf(payload, sizeof(payload), "p%d",  i);
        TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, topic, payload, -1, 0, false));
    }

    TEST_ASSERT_EQUAL_INT(32, bb_mqtt_client_host_pub_count(h));
    const bb_mqtt_client_host_pub_t *last = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("t/32", last->topic);

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_null_payload_is_safe(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", NULL, 0, 0, false));
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("", p->payload);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_explicit_len(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "hello world", 5, 0, false));
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(0, strncmp("hello", p->payload, 5));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_truncates_oversized_payload(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    static char big[600];
    memset(big, 'a', sizeof(big));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", big, (int)sizeof(big), 0, false));
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    // p->payload is 512 bytes; a 600-byte payload must be truncated to fit
    // (511 chars + NUL), not overrun the buffer.
    TEST_ASSERT_EQUAL_INT(511, (int)strlen(p->payload));
    bb_mqtt_client_destroy(h);
}

// B1-834 review CRITICAL-1: proves the host stub models esp-mqtt's real
// store/qos quirk (mqtt_client.c ~line 2299: an accepted qos=0 message is
// still reported as a generic reject when store=false) -- this is exactly
// the divergence that let bb_mqtt_client_enqueue() silently always fail on
// hardware while every (qos=0) host test passed, before the espidf backend
// was fixed to pass store=true.
void test_bb_mqtt_enqueue_qos0_store_false_returns_validation(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_enqueue_store(h, false);
    // Mutation check: drop the `qos == 0 && !h->store` condition (or invert
    // it) -> this would return BB_OK instead, masking the real hardware bug
    // the same way the pre-fix host tests did.
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION, bb_mqtt_client_enqueue(h, "t", "v", -1, 0, false));
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_enqueue_count(h));
    bb_mqtt_client_destroy(h);
}

// Same quirk does NOT apply to qos>0 (esp-mqtt always enqueues qos>0
// messages regardless of store) or when store=true (the default, matching
// the espidf backend's actual call site).
void test_bb_mqtt_enqueue_qos1_store_false_still_accepted(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_enqueue_store(h, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "v", -1, 1, false));
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_enqueue_count(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_qos0_store_true_is_accepted(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    // store defaults to true -- no setup call needed. Regression guard for
    // the default itself (matches the espidf backend's fixed call site).
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "v", -1, 0, false));
    bb_mqtt_client_destroy(h);
}

// Branch-coverage completion for the `qos == 0 && !h->store && msg_id == 0`
// guard: an outbox-full msg_id (-2) with qos=0/store=false must still take
// the msg_id!=0 path (the store/qos quirk does NOT override an explicit
// reject hook) and report BB_ERR_NO_SPACE, not BB_ERR_VALIDATION.
void test_bb_mqtt_enqueue_qos0_store_false_outbox_full_hook_still_no_space(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_enqueue_store(h, false);
    bb_mqtt_client_host_set_enqueue_msg_id(h, -2);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_enqueue(h, "t", "v", -1, 0, false));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_set_store_null_handle_is_safe(void)
{
    // Must not crash on NULL handle.
    bb_mqtt_client_host_set_enqueue_store(NULL, false);
}

void test_bb_mqtt_enqueue_set_outbox_limit_null_handle_is_safe(void)
{
    // Must not crash on NULL handle.
    bb_mqtt_client_host_set_outbox_limit(NULL, 8);
}

// B1-834 review CRITICAL-2: proves BB_ERR_NO_SPACE is actually reachable at
// a configured outbox cap, mirroring esp-mqtt's own outbox-limit gate
// (mqtt_client.c ~line 2280) that the espidf backend now wires up via
// CONFIG_BB_MQTT_CLIENT_OUTBOX_LIMIT_BYTES (esp_mqtt_client_config_t.outbox.limit).
void test_bb_mqtt_enqueue_outbox_limit_reached_returns_no_space(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 8);  // bytes

    // "hello" is 5 bytes -- fits under the 8-byte cap.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "hello", -1, 0, false));
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_enqueue_count(h));

    // A second 5-byte message pushes cumulative size to 10 > 8 -> rejected.
    // Mutation check: drop the outbox_limit check entirely (or invert the
    // comparison) -> this would return BB_OK, masking the unbounded-growth
    // bug the Kconfig cap exists to prevent.
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_enqueue(h, "t", "world", -1, 0, false));
    // A rejected enqueue must not be recorded as accepted.
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_enqueue_count(h));

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_outbox_limit_zero_is_unbounded(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    // Default (no bb_mqtt_client_host_set_outbox_limit call) is 0 =
    // unbounded, matching esp-mqtt's own default -- a large payload must
    // still be accepted.
    static char big[4096];
    memset(big, 'a', sizeof(big));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", big, (int)sizeof(big), 0, false));
    bb_mqtt_client_destroy(h);
}

// B1-834 review MEDIUM: proves BB_ERR_NO_SPACE is recoverable -- once the
// simulated outbox drains (mirroring esp-mqtt's own outbox shrinking as
// queued messages are sent/ACKed), an enqueue that would otherwise still be
// rejected succeeds again, matching the header's documented "retriable,
// back off and retry next tick" contract.
void test_bb_mqtt_enqueue_outbox_limit_reachable_again_after_drain(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 10);  // bytes

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "hello", -1, 0, false));  // 5 bytes -> 5
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "world", -1, 0, false));  // 5 bytes -> 10
    // A third message pushes cumulative size to 11 > 10 -> rejected.
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_enqueue(h, "t", "x", -1, 0, false));

    // Partial drain (3 of the 10 outstanding bytes) -- cumulative size drops
    // to 7, leaving exactly 3 bytes of headroom under the 10-byte cap. This
    // exercises the drain-helper's "still some outstanding" branch (bytes <
    // h->outbox_bytes), distinct from the full/over-drain branch covered by
    // test_bb_mqtt_enqueue_drain_outbox_floors_at_zero.
    bb_mqtt_client_host_drain_outbox(h, 3);

    // Mutation check: no-op the drain (or make it not decrement) -> this
    // would still return BB_ERR_NO_SPACE and the assertion fails.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "abc", -1, 0, false));  // 3 bytes -> 10
    TEST_ASSERT_EQUAL_INT(3, bb_mqtt_client_host_enqueue_count(h));

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_drain_outbox_floors_at_zero(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 4);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "ab", -1, 0, false));  // 2 bytes
    // Draining more than outstanding must floor at 0, not underflow.
    bb_mqtt_client_host_drain_outbox(h, 999);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "abcd", -1, 0, false));  // 4 bytes
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_enqueue_drain_outbox_null_handle_is_safe(void)
{
    // Must not crash on NULL handle.
    bb_mqtt_client_host_drain_outbox(NULL, 5);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_publish outbox tests (B1-834 review HIGH-2)
//
// esp_mqtt_client_publish() applies the SAME outbox_limit check as
// esp_mqtt_client_enqueue(), but gated on qos>0 (mqtt_client.c ~2181:
// "if (client->config->outbox_limit > 0 && qos > 0)"). CONFIG_BB_MQTT_CLIENT_
// OUTBOX_LIMIT_BYTES therefore makes -2/BB_ERR_NO_SPACE reachable from
// publish() too, not just enqueue().
// ---------------------------------------------------------------------------

void test_bb_mqtt_publish_qos1_outbox_limit_reached_returns_no_space(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 8);  // bytes

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_publish(h, "t", "hello", -1, 1, false));  // 5 bytes, qos=1
    // Mutation check: drop the `qos > 0` gate or the outbox check entirely
    // -> this would return BB_OK, masking the newly-reachable -2 path.
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_publish(h, "t", "world", -1, 1, false));  // 5 bytes, qos=1

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_qos0_ignores_outbox_limit(void)
{
    // qos=0 publishes never enter esp-mqtt's outbox (store=false in the
    // underlying mqtt_client_enqueue_publish() call) -- the outbox_limit
    // check is gated on qos>0 only, so a qos=0 publish must succeed even
    // when it would exceed the cap.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 4);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_publish(h, "t", "hello world", -1, 0, false));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_null_payload_qos1_is_safe(void)
{
    // Branch-coverage completion for the payload_len ternary the HIGH-2
    // outbox check reads (a NULL payload -> 0-byte contribution). qos=1 so
    // the outbox-limit path is actually exercised, not just skipped.
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_publish(h, "t", NULL, 0, 1, false));
    bb_mqtt_client_destroy(h);
}

// B1-834 review MEDIUM-2: proves len=0 with a non-NULL payload measures via
// strlen() -- matching esp-mqtt's own normalization ("if (len <= 0 && data
// != NULL) len = strlen(data)", mqtt_client.c:2177/2276) -- rather than
// contributing 0 bytes to the outbox boundary check. A prior version only
// auto-measured on len<0, so len=0 host-side undercounted vs hardware.
void test_bb_mqtt_enqueue_len_zero_measures_via_strlen_against_outbox_limit(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 4);  // "abcd" is 4 bytes -- exactly fits.
    // Mutation check: revert the `<= 0` normalization back to `< 0` -> len=0
    // would contribute 0 bytes instead of 4, and the second call below
    // (which must be rejected) would wrongly succeed instead.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "abcd", 0, 0, false));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_enqueue(h, "t", "x", 0, 0, false));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_len_zero_measures_via_strlen_against_outbox_limit(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 4);  // "abcd" is 4 bytes -- exactly fits.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_publish(h, "t", "abcd", 0, 1, false));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_publish(h, "t", "x", 0, 1, false));
    bb_mqtt_client_destroy(h);
}

// B1-834 review MEDIUM-3: proves the outbox counter is genuinely SHARED
// client-wide between publish() and enqueue() on one handle, as the code
// comments assert (bb_mqtt_client.c ~155-171, bb_mqtt_client.h ~404-417) --
// not two independent counters that happen to pass isolated per-API tests.
// Exercises both directions: enqueue() pushing publish() over the cap, and
// the reverse.
void test_bb_mqtt_enqueue_then_publish_share_outbox_limit(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 8);  // bytes

    // enqueue() (any qos) occupies 5 of the 8-byte cap.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_enqueue(h, "t", "hello", -1, 0, false));
    // Mutation check: give enqueue() and publish() independent counters ->
    // this qos>0 publish() would see an empty outbox and wrongly succeed.
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_publish(h, "t", "world", -1, 1, false));

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_then_enqueue_share_outbox_limit(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_set_outbox_limit(h, 8);  // bytes

    // publish(qos>0) occupies 5 of the 8-byte cap.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_publish(h, "t", "hello", -1, 1, false));
    // Mutation check: give enqueue() and publish() independent counters ->
    // this enqueue() would see an empty outbox and wrongly succeed.
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_mqtt_client_enqueue(h, "t", "world", -1, 0, false));

    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// B1-834 PR-1: pre-existing branch-coverage gaps closed while touching this
// file (coverage_baseline shrink-only ratchet -- "touch it, bring it to
// 100", scripts/coverage_baseline.py). None of these exercise NEW behavior
// added by this PR; they close branches in bb_mqtt_client_init/publish/
// get_stats/suspend_default/resume_default and the host test hooks that
// predate B1-834 but were never independently covered.
// ---------------------------------------------------------------------------

void test_bb_mqtt_init_null_uri_is_safe(void)
{
    // cfg->uri is optional at the host-stub layer (defensively guarded);
    // init must still succeed and leave the observability uri[] empty.
    bb_mqtt_client_cfg_t cfg = { .uri = NULL };
    bb_mqtt_client_t h = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_init(&cfg, &h));
    TEST_ASSERT_EQUAL_STRING("", bb_mqtt_client_host_last_uri(h));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_null_topic_returns_invalid_arg(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_publish(h, NULL, "v", -1, 0, false));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_publish_truncates_oversized_payload(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    static char big[600];
    memset(big, 'a', sizeof(big));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_publish(h, "t", big, (int)sizeof(big), 0, false));
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(511, (int)strlen(p->payload));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_get_stats_null_handle_returns_invalid_arg(void)
{
    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_get_stats(NULL, &stats));
}

void test_bb_mqtt_get_stats_null_out_returns_invalid_arg(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_get_stats(h, NULL));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_suspend_default_stop_only_no_handle_is_safe(void)
{
    // stop-only mode, no default handle set -- suspend must not crash and
    // must still flip the suspended flag.
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_host_set_stop_only(true);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_TRUE(bb_mqtt_client_host_is_suspended_default());
    // Cleanup.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    bb_mqtt_client_host_set_stop_only(false);
}

void test_bb_mqtt_resume_default_stop_only_no_handle_is_safe(void)
{
    // stop-only mode, suspended with no default handle -- resume must not
    // crash and must clear the suspended flag.
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_host_set_stop_only(true);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    TEST_ASSERT_FALSE(bb_mqtt_client_host_is_suspended_default());
    bb_mqtt_client_host_set_stop_only(false);
}

void test_bb_mqtt_host_last_pub_null_handle_returns_null(void)
{
    TEST_ASSERT_NULL(bb_mqtt_client_host_last_pub(NULL));
}

void test_bb_mqtt_host_pub_count_null_handle_returns_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_pub_count(NULL));
}

void test_bb_mqtt_host_last_uri_null_handle_returns_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", bb_mqtt_client_host_last_uri(NULL));
}

void test_bb_mqtt_host_set_connected_null_handle_is_safe(void)
{
    bb_mqtt_client_host_set_connected(NULL, true);
}

void test_bb_mqtt_host_reset_null_handle_is_safe(void)
{
    bb_mqtt_client_host_reset(NULL);
}

void test_bb_mqtt_host_set_subscribe_fail_null_handle_is_safe(void)
{
    bb_mqtt_client_host_set_subscribe_fail(NULL, true);
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

// B1-756: proves resume_default's bb_config_get_str reload actually resolves
// a persisted "uri" -- the prior test above only ever exercised the
// unconfigured-fallback branch (empty NVS -> "mqtt://localhost:1883"),
// never asserting that a genuinely-persisted value round-trips.
void test_bb_mqtt_resume_default_reloads_persisted_uri(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL));

    // Seed the literal "bb_mqtt"/"uri" address (the SAME one bb_nv used)
    // directly, as if a prior boot had persisted a broker URI.
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_mqtt_uri_field, "mqtt://broker.example.com:1883"));

    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_default_set(h);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_suspend_default());
    TEST_ASSERT_NULL(bb_mqtt_client_default());
    // h destroyed; full-release resume must recreate from the persisted uri.

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_resume_default());
    bb_mqtt_client_t h2 = bb_mqtt_client_default();
    TEST_ASSERT_NOT_NULL(h2);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.example.com:1883", bb_mqtt_client_host_last_uri(h2));

    bb_mqtt_client_stop_default();
    bb_storage_test_reset();
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

// Byte-identical guard (B1-949): bb_mqtt_client now owns these NVS
// namespace/key constants internally (bb_mqtt_client_nvs.h) instead of
// borrowing bb_nv's former BB_MQTT_NVS_NS/BB_NV_KEY_CLIENT_ID. If this test
// ever fails, do NOT "fix" the assertion -- the constant's value changed
// and that change must be reverted (strands provisioned-board MQTT config).
void test_bb_mqtt_client_nvs_ssot_values_are_byte_identical(void)
{
    TEST_ASSERT_EQUAL_STRING("bb_mqtt",   BB_MQTT_CLIENT_NVS_NS);
    TEST_ASSERT_EQUAL_STRING("client_id", BB_MQTT_CLIENT_NVS_KEY_CLIENT_ID);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_health_fill / bb_mqtt_client_health_desc (B1-1040):
// per-instance health -- deterministic single-threaded mutations against
// the mock clock, no real threads (see the test-runner rationale in
// test_bb_tcp_client.c -- a pthread-race test bit CI once already).
// ---------------------------------------------------------------------------

void test_bb_mqtt_health_connect_success_sets_connected_and_last_ok_ms(void)
{
    bb_mqtt_client_test_set_mock_time_ms64(12345);
    bb_mqtt_client_t h = make_client(NULL, NULL);

    bb_mqtt_client_host_simulate_connect_success(h);

    bb_mqtt_client_health_snap_t snap;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_health_fill(h, &snap));
    TEST_ASSERT_TRUE(snap.connected);
    TEST_ASSERT_EQUAL_INT64(12345, snap.last_ok_ms);
    TEST_ASSERT_EQUAL_UINT64(0, snap.fail_count);

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_health_disconnect_error_sets_disconnected_and_bumps_fail_count(void)
{
    bb_mqtt_client_test_set_mock_time_ms64(100);
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_simulate_connect_success(h);

    bb_mqtt_client_host_simulate_disconnect_error(h);

    bb_mqtt_client_health_snap_t snap;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_health_fill(h, &snap));
    TEST_ASSERT_FALSE(snap.connected);
    TEST_ASSERT_EQUAL_UINT64(1, snap.fail_count);

    bb_mqtt_client_destroy(h);
}

// fail_count is unconditional -- unlike reconnect_count, it also captures a
// failed FIRST connect attempt (never reached ever_connected).
void test_bb_mqtt_health_disconnect_error_before_ever_connected_still_bumps_fail_count(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);

    bb_mqtt_client_host_simulate_disconnect_error(h);

    bb_mqtt_client_health_snap_t snap;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_health_fill(h, &snap));
    TEST_ASSERT_FALSE(snap.connected);
    TEST_ASSERT_EQUAL_UINT64(1, snap.fail_count);

    bb_mqtt_client_stats_t stats;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_get_stats(h, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, stats.reconnect_count);  // gated on ever_connected -- unchanged

    bb_mqtt_client_destroy(h);
}

// bb_mqtt_client_host_simulate_clean_close() is a thin wrapper over the
// SAME bb_mqtt_client_priv_health_close() helper that bb_mqtt_client_destroy()
// calls on both backends (see bb_mqtt_client_health.h) -- this test therefore
// exercises the real clean-close code path, not a divergent stand-in
// (firmware-review finding, B1-1040). fail_count is seeded non-zero via a
// prior disconnect-error first, so the assertion proves the shared helper
// leaves it untouched rather than merely starting from zero.
void test_bb_mqtt_health_clean_close_sets_disconnected_without_fail_count_bump(void)
{
    bb_mqtt_client_test_set_mock_time_ms64(200);
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_simulate_connect_success(h);
    bb_mqtt_client_host_simulate_disconnect_error(h);  // seeds fail_count=1

    bb_mqtt_client_host_simulate_clean_close(h);

    bb_mqtt_client_health_snap_t snap;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_health_fill(h, &snap));
    TEST_ASSERT_FALSE(snap.connected);
    TEST_ASSERT_EQUAL_UINT64(1, snap.fail_count);  // a clean close is not a failure -- untouched
    TEST_ASSERT_EQUAL_INT64(200, snap.last_ok_ms);  // last successful stamp untouched

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_health_tls_error_code_set_from_tls_path_not_reset_on_success(void)
{
    bb_mqtt_client_test_set_mock_time_ms64(300);
    bb_mqtt_client_t h = make_client(NULL, NULL);

    bb_mqtt_client_host_set_tls_error_code(h, -0x7200);
    bb_mqtt_client_host_simulate_connect_success(h);

    bb_mqtt_client_health_snap_t snap;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_health_fill(h, &snap));
    TEST_ASSERT_TRUE(snap.connected);
    TEST_ASSERT_EQUAL_INT64(-0x7200, snap.tls_error_code);  // untouched by a later success

    bb_mqtt_client_destroy(h);
}

// Per-instance isolation: two independently-created handles never cross-talk
// their health state.
void test_bb_mqtt_health_per_instance_isolation(void)
{
    bb_mqtt_client_test_set_mock_time_ms64(10);
    bb_mqtt_client_t a = make_client(NULL, NULL);
    bb_mqtt_client_t b = make_client(NULL, NULL);

    bb_mqtt_client_host_simulate_connect_success(a);
    bb_mqtt_client_host_simulate_disconnect_error(b);

    bb_mqtt_client_health_snap_t snap_a, snap_b;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_health_fill(a, &snap_a));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_health_fill(b, &snap_b));

    TEST_ASSERT_TRUE(snap_a.connected);
    TEST_ASSERT_EQUAL_UINT64(0, snap_a.fail_count);
    TEST_ASSERT_FALSE(snap_b.connected);
    TEST_ASSERT_EQUAL_UINT64(1, snap_b.fail_count);

    bb_mqtt_client_destroy(a);
    bb_mqtt_client_destroy(b);
}

void test_bb_mqtt_health_fill_null_handle_returns_invalid_arg(void)
{
    bb_mqtt_client_health_snap_t snap;
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_health_fill(NULL, &snap));
}

void test_bb_mqtt_health_fill_null_out_returns_invalid_arg(void)
{
    bb_mqtt_client_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_client_health_fill(h, NULL));
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_health_simulate_connect_success_null_handle_is_safe(void)
{
    bb_mqtt_client_host_simulate_connect_success(NULL);
}

void test_bb_mqtt_health_simulate_disconnect_error_null_handle_is_safe(void)
{
    bb_mqtt_client_host_simulate_disconnect_error(NULL);
}

void test_bb_mqtt_health_simulate_clean_close_null_handle_is_safe(void)
{
    bb_mqtt_client_host_simulate_clean_close(NULL);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_health_desc: the descriptor produces exactly the 4
// documented wire keys, in order, against a live-filled snapshot.
// ---------------------------------------------------------------------------

typedef struct {
    char keys[8][32];
    int  count;
} mqtt_health_desc_capture_t;

static void mqtt_health_capture_emit_bool(void *ctx, const char *key, bool v)
{
    (void)v;
    mqtt_health_desc_capture_t *cap = (mqtt_health_desc_capture_t *)ctx;
    strncpy(cap->keys[cap->count], key, sizeof(cap->keys[0]) - 1);
    cap->count++;
}

static void mqtt_health_capture_emit_i64(void *ctx, const char *key, int64_t v)
{
    (void)v;
    mqtt_health_desc_capture_t *cap = (mqtt_health_desc_capture_t *)ctx;
    strncpy(cap->keys[cap->count], key, sizeof(cap->keys[0]) - 1);
    cap->count++;
}

static void mqtt_health_capture_emit_u64(void *ctx, const char *key, uint64_t v)
{
    (void)v;
    mqtt_health_desc_capture_t *cap = (mqtt_health_desc_capture_t *)ctx;
    strncpy(cap->keys[cap->count], key, sizeof(cap->keys[0]) - 1);
    cap->count++;
}

void test_bb_mqtt_health_desc_walks_all_four_keys(void)
{
    bb_mqtt_client_test_set_mock_time_ms64(555);
    bb_mqtt_client_t h = make_client(NULL, NULL);
    bb_mqtt_client_host_simulate_connect_success(h);

    bb_mqtt_client_health_snap_t snap;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_health_fill(h, &snap));

    mqtt_health_desc_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    bb_serialize_emit_t emit = {
        .format_id = BB_FORMAT_NONE,
        .ctx       = &cap,
        .emit_bool = mqtt_health_capture_emit_bool,
        .emit_i64  = mqtt_health_capture_emit_i64,
        .emit_u64  = mqtt_health_capture_emit_u64,
    };
    bb_serialize_walk(&bb_mqtt_client_health_desc, &snap, &emit);

    TEST_ASSERT_EQUAL_INT(4, cap.count);
    TEST_ASSERT_EQUAL_STRING("connected", cap.keys[0]);
    TEST_ASSERT_EQUAL_STRING("last_ok_ms", cap.keys[1]);
    TEST_ASSERT_EQUAL_STRING("fail_count", cap.keys[2]);
    TEST_ASSERT_EQUAL_STRING("tls_error_code", cap.keys[3]);

    bb_mqtt_client_destroy(h);
}
