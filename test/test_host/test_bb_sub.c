// Tests for bb_sub (B1-490): cache-only ingress routing core.
//
// Covers:
//  - dynamic per-topic bb_cache registration + round-trip via
//    bb_cache_get_serialized
//  - multiple distinct topics registered independently
//  - seen-topics registry overflow (BB_SUB_MAX_TOPICS) drops + counts,
//    without disturbing already-registered topics
//  - oversized-payload rejection (BB_SUB_MAX_PAYLOAD_BYTES)
//  - NULL-arg validation
//  - aggregate change-notification event (BB_SUB_EVENT_TOPIC)
#include "unity.h"
#include "bb_sub.h"
#include "bb_cache.h"
#include "bb_event.h"
#include "bb_event_test.h"
#include "bb_json.h"
#include "bb_mem_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Declared in bb_cache_espidf.c under BB_CACHE_TESTING; not in the public
// header (mirrors test_bb_cache_fidelity.c).
void bb_cache_reset_for_test(void);

// ---------------------------------------------------------------------------
// setUp
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    bb_cache_reset_for_test();
    bb_sub_reset_for_test();
    bb_event_reset_for_test();
    bb_event_init(NULL);
}

// ---------------------------------------------------------------------------
// Dynamic registration + round-trip
// ---------------------------------------------------------------------------

void test_bb_sub_route_registers_and_cache_reflects_payload(void)
{
    reset_all();

    const char *payload = "{\"a\":1,\"b\":\"x\"}";
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("metrics/host1/meta", payload, strlen(payload)));

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("metrics/host1/meta", buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(len > 0);

    bb_json_t obj = bb_json_parse(buf, len);
    TEST_ASSERT_NOT_NULL(obj);
    double a = 0;
    char bstr[16] = {0};
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj, "a", &a));
    TEST_ASSERT_EQUAL_INT(1, (int)a);
    TEST_ASSERT_TRUE(bb_json_obj_get_string(obj, "b", bstr, sizeof(bstr)));
    TEST_ASSERT_EQUAL_STRING("x", bstr);
    bb_json_free(obj);
}

void test_bb_sub_route_second_call_updates_same_topic(void)
{
    reset_all();

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("t1", "{\"n\":1}", strlen("{\"n\":1}")));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("t1", "{\"n\":2}", strlen("{\"n\":2}")));

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("t1", buf, sizeof(buf), &len));

    bb_json_t obj = bb_json_parse(buf, len);
    TEST_ASSERT_NOT_NULL(obj);
    double n = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj, "n", &n));
    TEST_ASSERT_EQUAL_INT(2, (int)n);
    bb_json_free(obj);
}

void test_bb_sub_route_multiple_distinct_topics(void)
{
    reset_all();

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("topic/a", "{\"v\":\"aa\"}", strlen("{\"v\":\"aa\"}")));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("topic/b", "{\"v\":\"bb\"}", strlen("{\"v\":\"bb\"}")));

    char buf_a[128], buf_b[128];
    size_t len_a = 0, len_b = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("topic/a", buf_a, sizeof(buf_a), &len_a));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("topic/b", buf_b, sizeof(buf_b), &len_b));

    bb_json_t oa = bb_json_parse(buf_a, len_a);
    bb_json_t ob = bb_json_parse(buf_b, len_b);
    char va[8] = {0}, vb[8] = {0};
    TEST_ASSERT_TRUE(bb_json_obj_get_string(oa, "v", va, sizeof(va)));
    TEST_ASSERT_TRUE(bb_json_obj_get_string(ob, "v", vb, sizeof(vb)));
    TEST_ASSERT_EQUAL_STRING("aa", va);
    TEST_ASSERT_EQUAL_STRING("bb", vb);
    bb_json_free(oa);
    bb_json_free(ob);
}

// ---------------------------------------------------------------------------
// Overflow handling
// ---------------------------------------------------------------------------

void test_bb_sub_route_overflow_drops_and_counts(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_UINT32(0, bb_sub_dropped_count());

    char topic[32];
    for (int i = 0; i < BB_SUB_MAX_TOPICS; i++) {
        snprintf(topic, sizeof(topic), "fill.%d", i);
        bb_err_t rc = bb_sub_route(topic, "{\"i\":1}", strlen("{\"i\":1}"));
        TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, rc, topic);
    }
    TEST_ASSERT_EQUAL_UINT32(0, bb_sub_dropped_count());

    // One more distinct topic must be dropped.
    bb_err_t rc = bb_sub_route("fill.overflow", "{\"i\":1}", strlen("{\"i\":1}"));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT32(1, bb_sub_dropped_count());

    // Already-registered topics keep working normally.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("fill.0", "{\"i\":2}", strlen("{\"i\":2}")));
    TEST_ASSERT_EQUAL_UINT32(1, bb_sub_dropped_count());
}

// ---------------------------------------------------------------------------
// Oversized payload
// ---------------------------------------------------------------------------

void test_bb_sub_route_oversized_payload_returns_no_space(void)
{
    reset_all();

    char *big = (char *)malloc(BB_SUB_MAX_PAYLOAD_BYTES + 64);
    TEST_ASSERT_NOT_NULL(big);
    big[0] = '{';
    memset(big + 1, 'a', BB_SUB_MAX_PAYLOAD_BYTES + 62);
    big[BB_SUB_MAX_PAYLOAD_BYTES + 62] = '}';
    big[BB_SUB_MAX_PAYLOAD_BYTES + 63] = '\0';

    bb_err_t rc = bb_sub_route("too.big", big, strlen(big));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_UINT32(1, bb_sub_dropped_count());

    free(big);
}

// ---------------------------------------------------------------------------
// Passthrough serializer edge cases — malformed JSON / non-object top level.
// Both leave the cache object untouched (serializes as {}), same as an
// empty payload.
// ---------------------------------------------------------------------------

void test_bb_sub_route_malformed_json_payload_serializes_empty(void)
{
    reset_all();

    const char *payload = "{not valid json";
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("bad.json", payload, strlen(payload)));

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("bad.json", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING("{}", buf);
}

void test_bb_sub_route_non_object_json_payload_serializes_empty(void)
{
    reset_all();

    const char *payload = "[1,2,3]";
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("array.payload", payload, strlen(payload)));

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("array.payload", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING("{}", buf);
}

// ---------------------------------------------------------------------------
// bb_cache registry full (separate cap from bb_sub's own seen-topics
// registry) — bb_sub still has room, bb_cache does not.
// ---------------------------------------------------------------------------

static void dummy_cache_serialize(bb_json_t obj, const void *snap)
{
    (void)obj; (void)snap;
}

void test_bb_sub_route_bb_cache_registry_full_returns_error(void)
{
    reset_all();

    // Fill bb_cache's own registry directly (BB_CACHE_MAX_TOPICS=32 in the
    // native test env) with topics bb_sub never sees, so bb_sub's local
    // seen-topics registry is still empty when bb_sub_route runs below.
    // Each topic string must outlive the registry (bb_cache stores the
    // pointer, not a copy — see bb_cache.h) so each gets its own heap
    // buffer, not a reused stack buffer.
    char *topics[BB_CACHE_MAX_TOPICS];
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        topics[i] = (char *)malloc(32);
        TEST_ASSERT_NOT_NULL(topics[i]);
        snprintf(topics[i], 32, "cachefill.%d", i);
        bb_err_t rc = bb_cache_register(topics[i], NULL, 8, dummy_cache_serialize);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, rc, topics[i]);
    }

    bb_err_t rc = bb_sub_route("new.topic.not.in.cache", "{}", strlen("{}"));
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1, bb_sub_dropped_count());

    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) free(topics[i]);
}

// ---------------------------------------------------------------------------
// Oversized topic — rejected (no silent truncation/collision).
// ---------------------------------------------------------------------------

void test_bb_sub_route_oversized_topic_returns_invalid_arg(void)
{
    reset_all();

    // BB_SUB_TOPIC_MAX is 96 (private to bb_sub.c); 100 chars is over the
    // limit regardless.
    char topic[128];
    memset(topic, 'x', sizeof(topic) - 1);
    topic[sizeof(topic) - 1] = '\0';

    bb_err_t rc = bb_sub_route(topic, "{}", strlen("{}"));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
    TEST_ASSERT_EQUAL_UINT32(0, bb_sub_dropped_count());

    // Never got far enough to touch bb_cache.
    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_cache_get_serialized(topic, buf, sizeof(buf), &len));
}

// ---------------------------------------------------------------------------
// Zero-length payload — stored as a valid empty payload, not measured via
// strlen (a real MQTT retain-delete has no NUL terminator guarantee).
// ---------------------------------------------------------------------------

void test_bb_sub_route_zero_length_payload_stored_as_empty(void)
{
    reset_all();

    bb_err_t rc = bb_sub_route("empty.payload", "", 0);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0, bb_sub_dropped_count());

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("empty.payload", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING("{}", buf);
}

// ---------------------------------------------------------------------------
// Snapshot heap-allocation failure.
// ---------------------------------------------------------------------------

#ifdef BB_MEM_TESTING
// Fail-injection hook: always returns NULL. Scoped tightly around the single
// bb_sub_route() call below — bb_sub's only bb_mem allocation for a message
// on a topic that is already bb_cache-registered is the per-message snapshot
// (bb_cache_register_ex / bb_cache itself never call through bb_mem), so this
// cannot spuriously fail an unrelated allocation in the same test.
static void *s_always_fail_malloc(size_t sz)
{
    (void)sz;
    return NULL;
}
#endif

void test_bb_sub_route_snap_alloc_failure_returns_no_mem(void)
{
#ifdef BB_MEM_TESTING
    reset_all();

    // Pre-register the topic with a real allocation so only the snapshot
    // alloc under test happens while the fail hook is installed.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("alloc.fail", "{\"a\":0}", strlen("{\"a\":0}")));

    bb_mem_set_alloc_hook(s_always_fail_malloc);
    bb_err_t rc = bb_sub_route("alloc.fail", "{\"a\":1}", strlen("{\"a\":1}"));
    bb_mem_set_alloc_hook(NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_MEM, rc);
    TEST_ASSERT_EQUAL_UINT32(1, bb_sub_dropped_count());
#else
    TEST_IGNORE_MESSAGE("BB_MEM_TESTING not enabled");
#endif
}

// ---------------------------------------------------------------------------
// Aggregate event topic registration failure (bb_event registry full) —
// routing still succeeds; the aggregate notification is just not posted.
// ---------------------------------------------------------------------------

void test_bb_sub_route_aggregate_topic_register_failure_still_routes(void)
{
    reset_all();

    // Fill bb_event's topic registry (CONFIG_BB_EVENT_MAX_TOPICS=64 in the
    // native test env) so ensure_event_topic()'s bb_event_topic_register
    // call for BB_SUB_EVENT_TOPIC fails.
    char name[32];
    bb_event_topic_t t;
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof(name), "evfill.%d", i);
        bb_err_t rc = bb_event_topic_register(name, &t);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, rc, name);
    }

    bb_err_t rc = bb_sub_route("routes.anyway", "{\"a\":1}", strlen("{\"a\":1}"));
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("routes.anyway", buf, sizeof(buf), &len));
}

// ---------------------------------------------------------------------------
// NULL / invalid arg validation
// ---------------------------------------------------------------------------

void test_bb_sub_route_null_topic_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sub_route(NULL, "{}", 0));
}

void test_bb_sub_route_empty_topic_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sub_route("", "{}", 0));
}

void test_bb_sub_route_null_payload_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sub_route("t", NULL, 0));
}

// ---------------------------------------------------------------------------
// Aggregate change-notification event
// ---------------------------------------------------------------------------

static int  s_agg_calls = 0;
static char s_agg_last_topic[128] = {0};

static void agg_handler(bb_event_topic_t topic, int32_t id,
                        const void *data, size_t size, void *user)
{
    (void)topic; (void)id; (void)user;
    s_agg_calls++;
    if (data && size > 0 && size < sizeof(s_agg_last_topic)) {
        memcpy(s_agg_last_topic, data, size);
    }
}

static void reset_agg(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_all();
    s_agg_calls = 0;
    s_agg_last_topic[0] = '\0';
}

void test_bb_sub_route_emits_aggregate_event(void)
{
    reset_agg();

    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_subscribe(agg_handler, NULL, &sub));
    TEST_ASSERT_NOT_NULL(sub);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("metrics/host2/meta", "{\"x\":1}", strlen("{\"x\":1}")));
    bb_event_pump(0);

    TEST_ASSERT_EQUAL_INT(1, s_agg_calls);
    TEST_ASSERT_EQUAL_STRING("metrics/host2/meta", s_agg_last_topic);

    bb_event_unsubscribe(sub);
}

void test_bb_sub_subscribe_before_any_route_call(void)
{
    reset_agg();

    // Subscribing before any bb_sub_route() call must still succeed — the
    // aggregate topic is registered lazily by bb_sub_subscribe() itself.
    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_subscribe(agg_handler, NULL, &sub));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_route("t.early", "{}", strlen("{}")));
    bb_event_pump(0);

    TEST_ASSERT_EQUAL_INT(1, s_agg_calls);
    TEST_ASSERT_EQUAL_STRING("t.early", s_agg_last_topic);

    bb_event_unsubscribe(sub);
}

void test_bb_sub_route_dropped_message_does_not_emit_aggregate_event(void)
{
    reset_agg();

    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_subscribe(agg_handler, NULL, &sub));

    bb_err_t rc = bb_sub_route("t", NULL, 0);   // invalid arg, never reaches routing
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
    bb_event_pump(0);

    TEST_ASSERT_EQUAL_INT(0, s_agg_calls);

    bb_event_unsubscribe(sub);
}

void test_bb_sub_subscribe_null_cb_returns_invalid_arg(void)
{
    reset_all();
    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sub_subscribe(NULL, NULL, &sub));
}

void test_bb_sub_subscribe_null_out_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sub_subscribe(agg_handler, NULL, NULL));
}

void test_bb_sub_subscribe_aggregate_topic_register_failure_returns_invalid_state(void)
{
    reset_all();

    char name[32];
    bb_event_topic_t t;
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof(name), "subfill.%d", i);
        bb_err_t rc = bb_event_topic_register(name, &t);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, rc, name);
    }

    bb_event_sub_t sub = NULL;
    bb_err_t rc = bb_sub_subscribe(agg_handler, NULL, &sub);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, rc);
}
