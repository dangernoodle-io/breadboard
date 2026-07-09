// Tests for bb_mqtt_client_on_message / bb_mqtt_client_host_inject_message (B1-487): the
// per-handle receive-callback primitive that an MQTT ingress adapter sits on top of.
#include "unity.h"
#include "bb_mqtt_client.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Capture helpers
// ---------------------------------------------------------------------------

static int    s_calls = 0;
static char   s_last_topic[128] = {0};
static char   s_last_payload[256] = {0};
static size_t s_last_len = 0;
static void  *s_last_ctx = NULL;

static void capture_cb(const char *topic, const void *payload, size_t len, void *ctx)
{
    s_calls++;
    s_last_ctx = ctx;
    s_last_len = len;
    snprintf(s_last_topic, sizeof(s_last_topic), "%s", topic);
    memset(s_last_payload, 0, sizeof(s_last_payload));
    if (payload && len > 0 && len < sizeof(s_last_payload)) {
        memcpy(s_last_payload, payload, len);
    }
}

static void reset_capture(void)
{
    s_calls = 0;
    s_last_topic[0]   = '\0';
    s_last_payload[0] = '\0';
    s_last_len = 0;
    s_last_ctx = NULL;
}

static bb_mqtt_client_t make_handle(void)
{
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_client_t h = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    TEST_ASSERT_NOT_NULL(h);
    return h;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_on_message_receives_injected_message(void)
{
    reset_capture();
    bb_mqtt_client_t h = make_handle();
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_on_message(h, capture_cb, NULL));

    bb_mqtt_client_host_inject_message(h, "test/topic", "hello", 5);

    TEST_ASSERT_EQUAL_INT(1, s_calls);
    TEST_ASSERT_EQUAL_STRING("test/topic", s_last_topic);
    TEST_ASSERT_EQUAL_INT(5, (int)s_last_len);
    TEST_ASSERT_EQUAL_STRING("hello", s_last_payload);

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_on_message_passes_ctx(void)
{
    reset_capture();
    bb_mqtt_client_t h = make_handle();
    int marker = 42;
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_on_message(h, capture_cb, &marker));

    bb_mqtt_client_host_inject_message(h, "t", "v", 1);

    TEST_ASSERT_EQUAL_PTR(&marker, s_last_ctx);

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_on_message_multiple_injections(void)
{
    reset_capture();
    bb_mqtt_client_t h = make_handle();
    bb_mqtt_client_on_message(h, capture_cb, NULL);

    bb_mqtt_client_host_inject_message(h, "a", "1", 1);
    bb_mqtt_client_host_inject_message(h, "b", "22", 2);

    TEST_ASSERT_EQUAL_INT(2, s_calls);
    TEST_ASSERT_EQUAL_STRING("b", s_last_topic);
    TEST_ASSERT_EQUAL_STRING("22", s_last_payload);

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_on_message_replaces_previous_callback(void)
{
    reset_capture();
    bb_mqtt_client_t h = make_handle();
    bb_mqtt_client_on_message(h, capture_cb, NULL);
    bb_mqtt_client_host_inject_message(h, "first", "1", 1);
    TEST_ASSERT_EQUAL_INT(1, s_calls);

    // Registering NULL clears the slot — subsequent injects are no-ops.
    bb_mqtt_client_on_message(h, NULL, NULL);
    bb_mqtt_client_host_inject_message(h, "second", "2", 1);
    TEST_ASSERT_EQUAL_INT(1, s_calls);   // unchanged

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_on_message_no_callback_registered_is_safe(void)
{
    reset_capture();
    bb_mqtt_client_t h = make_handle();
    // No callback registered at all — must not crash.
    bb_mqtt_client_host_inject_message(h, "t", "v", 1);
    TEST_ASSERT_EQUAL_INT(0, s_calls);

    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_on_message_null_handle_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_mqtt_client_on_message(NULL, capture_cb, NULL));
}

// Per-handle capture context for the interleaved cross-talk test below —
// distinct from the shared s_* globals used by the other tests in this file.
typedef struct {
    int    calls;
    char   topic[128];
    char   payload[256];
    size_t len;
} handle_capture_t;

static void handle_capture_cb(const char *topic, const void *payload, size_t len, void *ctx)
{
    handle_capture_t *cap = (handle_capture_t *)ctx;
    cap->calls++;
    snprintf(cap->topic, sizeof(cap->topic), "%s", topic);
    memset(cap->payload, 0, sizeof(cap->payload));
    if (payload && len > 0 && len < sizeof(cap->payload)) {
        memcpy(cap->payload, payload, len);
    }
    cap->len = len;
}

// HIGH-2 regression: two independent handles must never splice bytes across
// each other's reassembly state — each has its own callback + buffer. This
// exercises two CONCURRENTLY ACTIVE handles: h1's multi-fragment message is
// left partially reassembled while h2's complete message is injected and
// fires, then h1 is completed — proving neither handle's bytes leak into
// the other's callback regardless of interleaving order.
void test_bb_mqtt_on_message_two_handles_do_not_cross_talk(void)
{
    bb_mqtt_client_t h1 = make_handle();
    bb_mqtt_client_t h2 = make_handle();

    handle_capture_t ctx1 = {0};
    handle_capture_t ctx2 = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_on_message(h1, handle_capture_cb, &ctx1));
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_on_message(h2, handle_capture_cb, &ctx2));

    // h1: start a 3-fragment message on topic "a/x" (total 9 bytes: "AAABBBCCC"),
    // but only deliver 2 of 3 fragments — leaves h1's reassembly state active
    // and partially filled while h2 gets driven below.
    bb_mqtt_client_host_inject_fragment(h1, "a/x", 9, 0, "AAA", 3);
    bb_mqtt_client_host_inject_fragment(h1, "a/x", 9, 3, "BBB", 3);
    TEST_ASSERT_EQUAL_INT(0, ctx1.calls);
    TEST_ASSERT_EQUAL_INT(0, ctx2.calls);

    // h2: inject a complete, distinct message ("b/y" / "zzzzz", different
    // length and content from h1's in-flight payload) while h1 is mid-stream.
    bb_mqtt_client_host_inject_message(h2, "b/y", "zzzzz", 5);

    // h2 fired exactly once with exactly its own topic+payload; h1 has not
    // fired at all yet — no splice from h1's partial state into h2's result.
    TEST_ASSERT_EQUAL_INT(1, ctx2.calls);
    TEST_ASSERT_EQUAL_STRING("b/y", ctx2.topic);
    TEST_ASSERT_EQUAL_INT(5, (int)ctx2.len);
    TEST_ASSERT_EQUAL_MEMORY("zzzzz", ctx2.payload, 5);
    TEST_ASSERT_EQUAL_INT(0, ctx1.calls);

    // Complete h1's message with its final fragment.
    bb_mqtt_client_host_inject_fragment(h1, "a/x", 9, 6, "CCC", 3);

    // h1 fired exactly once with its full concatenated payload — no bytes
    // from h2's message spliced in either direction. h2's capture is
    // unchanged (still exactly one call, from its own message).
    TEST_ASSERT_EQUAL_INT(1, ctx1.calls);
    TEST_ASSERT_EQUAL_STRING("a/x", ctx1.topic);
    TEST_ASSERT_EQUAL_INT(9, (int)ctx1.len);
    TEST_ASSERT_EQUAL_MEMORY("AAABBBCCC", ctx1.payload, 9);
    TEST_ASSERT_EQUAL_INT(1, ctx2.calls);

    bb_mqtt_client_destroy(h1);
    bb_mqtt_client_destroy(h2);
}
