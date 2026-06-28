// bb_pub telemetry SSOT fidelity test.
//
// Proves the serialize-once-per-generation guarantee:
//   - On a tick with both BB_PUB_TELEM_SSE and BB_PUB_TELEM_SINKS flags,
//     the serialize function is called ONCE.  SSE and the sink receive the same
//     pre-serialized string — no second serialization.
//   - A REST-side bb_cache_serialize_into call (serving the last snapshot) is a
//     separate invocation: the count increments to 2 after the REST read,
//     confirming the cache holds the frozen snap rather than re-gathering.
//
// Topology for each test:
//   bb_pub_register_telemetry("telem_fid", gather, serialize, snap_size,
//                              BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS)
//   1 capturing sink
//   bb_pub_tick_once()

#include "unity.h"
#include "bb_pub.h"
#include "bb_cache.h"
#include "bb_json.h"
#include "bb_nv.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Shared state — reset before each test
// ---------------------------------------------------------------------------

void bb_cache_reset_for_test(void);   /* declared in bb_cache_espidf.c (BB_CACHE_TESTING) */

static int   s_gather_count;
static int   s_serialize_count;
static int   s_sink_count;

// ---------------------------------------------------------------------------
// Snapshot struct and functions
// ---------------------------------------------------------------------------

typedef struct {
    int value;
} fid_snap_t;

static bool fid_gather(void *snap_buf, void *ctx)
{
    (void)ctx;
    s_gather_count++;
    fid_snap_t *snap = snap_buf;
    snap->value = 42;
    return true;
}

static void fid_serialize(bb_json_t obj, const void *snap_raw)
{
    s_serialize_count++;
    const fid_snap_t *snap = snap_raw;
    bb_json_obj_set_int(obj, "value", (int64_t)snap->value);
}

// ---------------------------------------------------------------------------
// Capturing sink
// ---------------------------------------------------------------------------

#define FID_CAPTURE_CAP 8

typedef struct { char topic[128]; char payload[256]; } fid_entry_t;
static fid_entry_t s_fid_captured[FID_CAPTURE_CAP];

static bb_err_t fid_publish(void *ctx, const char *topic,
                             const char *payload, int len)
{
    (void)ctx;
    (void)len;
    if (s_sink_count >= FID_CAPTURE_CAP) return BB_ERR_NO_SPACE;
    fid_entry_t *e = &s_fid_captured[s_sink_count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static void fid_reset(void)
{
    bb_pub_test_reset();
    bb_cache_reset_for_test();
    bb_nv_config_set_hostname("testhost");
    s_gather_count    = 0;
    s_serialize_count = 0;
    s_sink_count      = 0;
    memset(s_fid_captured, 0, sizeof(s_fid_captured));
}

static void fid_register_and_add_sink(bb_pub_telemetry_flags_t flags)
{
    bb_pub_telemetry_cfg_t cfg = {
        .topic     = "telem_fid",
        .gather    = fid_gather,
        .serialize = fid_serialize,
        .snap_size = sizeof(fid_snap_t),
        .flags     = flags,
        .ctx       = NULL,
    };
    bb_err_t err = bb_pub_register_telemetry(&cfg);
    TEST_ASSERT_EQUAL_INT(0 /* BB_OK */, err);

    bb_pub_sink_t sink = { .publish = fid_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. Serialize is called exactly once per tick when BB_PUB_TELEM_SINKS is set.
void test_bb_pub_telemetry_fidelity_serialize_once_per_tick_sinks(void)
{
    fid_reset();
    fid_register_and_add_sink(BB_PUB_TELEM_SINKS);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_gather_count,
        "gather must be called once per tick");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_serialize_count,
        "serialize must be called once per tick (SSOT guarantee)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_sink_count,
        "sink must receive one delivery");
}

// 2. Serialize is called exactly once per tick when BB_PUB_TELEM_SSE is set
//    (SSE post shares the same serialized string, no second serialization).
void test_bb_pub_telemetry_fidelity_serialize_once_per_tick_sse(void)
{
    fid_reset();
    fid_register_and_add_sink(BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_gather_count,
        "gather must be called once per tick");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_serialize_count,
        "serialize must be called once even with SSE + sinks (SSOT)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_sink_count,
        "sink must receive one delivery");
}

// 3. REST reads via bb_cache_get_serialized are MEMOIZED: after the tick, no
//    matter how many times REST polls, the serializer does not run again and
//    gather never re-runs. A new generation (next tick) bumps the count by one.
void test_bb_pub_telemetry_fidelity_rest_read_no_regather(void)
{
    fid_reset();
    fid_register_and_add_sink(BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS);

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_serialize_count);
    TEST_ASSERT_EQUAL_INT(1, s_gather_count);

    // Three REST polls via the memoized copy-out accessor.
    char   j[512];
    size_t l = 0;
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid", j, sizeof(j), &l));
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_serialize_count,
        "REST polls reuse memoized bytes — serializer stays at 1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_gather_count,
        "gather must NOT re-run on REST reads");

    // A second tick is a new generation — exactly one more serialize.
    bb_pub_tick_once();
    bb_cache_get_serialized("telem_fid", j, sizeof(j), &l);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_serialize_count,
        "second generation triggers exactly one more serialize");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_gather_count,
        "second tick gathers again");
}

// 4. Gather not called when tick is skipped (no sinks, no SSE consumers).
void test_bb_pub_telemetry_fidelity_no_gather_when_tick_not_fired(void)
{
    fid_reset();
    // Register telemetry but with SINKS flag; do NOT register any sink.
    bb_pub_telemetry_cfg_t cfg = {
        .topic     = "telem_fid",
        .gather    = fid_gather,
        .serialize = fid_serialize,
        .snap_size = sizeof(fid_snap_t),
        .flags     = BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
    };
    TEST_ASSERT_EQUAL_INT(0, bb_pub_register_telemetry(&cfg));

    // No sink registered → nothing to deliver → gather should still fire
    // (the telem gather runs whenever snap_sink_count > 0 for SINKS or
    // always for SSE, but with SINKS only + 0 sinks the gather is skipped).
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_gather_count,
        "gather must not fire when no sinks are registered for SINKS-only topic");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_serialize_count,
        "serialize must not fire when gather did not fire");
}

// 5. Payload content correctness — the sink receives the serialized snap value.
void test_bb_pub_telemetry_fidelity_payload_value_correct(void)
{
    fid_reset();
    fid_register_and_add_sink(BB_PUB_TELEM_SINKS);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_sink_count);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_fid_captured[0].payload, "\"value\""),
        "payload must contain 'value' field");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_fid_captured[0].payload, "42"),
        "payload must contain the gathered value (42)");
}

// 6. Byte-identity: the bytes REST reads (via bb_cache_get_serialized) are the
//    EXACT same bytes the sink received. SSE uses bb_cache_post_serialized with
//    that same string, so REST == SSE == sink by construction. The telem path
//    no longer post-injects uptime_ms — all fields come from the serializer.
void test_bb_pub_telemetry_fidelity_rest_equals_sink_bytes(void)
{
    fid_reset();
    fid_register_and_add_sink(BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS);

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_sink_count);

    char   rest[512];
    size_t rlen = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid", rest, sizeof(rest), &rlen));

    // REST read after the tick is memoized — still one serialize total.
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_serialize_count,
        "REST read reuses memoized bytes — no extra serialize");
    // REST bytes == the bytes the sink received.
    TEST_ASSERT_EQUAL_STRING(rest, s_fid_captured[0].payload);
    // No post-injected uptime_ms on the telem path.
    TEST_ASSERT_NULL_MESSAGE(strstr(rest, "\"uptime_ms\""),
        "telem path must not post-inject uptime_ms");
}

// 7. Serialize-once-per-generation, exercised directly against bb_cache:
//    update once → get_serialized 3x → serializer ran exactly once; a second
//    update bumps the next read to two.
void test_bb_pub_telemetry_fidelity_cache_serialize_once_per_generation(void)
{
    fid_reset();
    bb_err_t rc = bb_cache_register_ex("telem_fid_cache", NULL,
                                       sizeof(fid_snap_t), fid_serialize,
                                       BB_CACHE_FLAG_NONE);
    TEST_ASSERT_EQUAL_INT(0, rc);

    fid_snap_t s = { .value = 7 };
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update("telem_fid_cache", &s));

    char   j1[256];
    size_t l1 = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid_cache", j1, sizeof(j1), &l1));
    TEST_ASSERT_EQUAL_INT(1, s_serialize_count);

    // Two more reads — memoized, no re-serialize; same bytes each time.
    char   j2[256];
    size_t l2 = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid_cache", j2, sizeof(j2), &l2));
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid_cache", j2, sizeof(j2), &l2));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_serialize_count,
        "serializer must run at most once per update generation");
    TEST_ASSERT_EQUAL_STRING(j1, j2);

    // New generation: update bumps the next read to exactly two.
    s.value = 9;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update("telem_fid_cache", &s));
    char   j3[256];
    size_t l3 = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid_cache", j3, sizeof(j3), &l3));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_serialize_count,
        "a second update triggers exactly one more serialize");
    TEST_ASSERT_NOT_NULL(strstr(j3, "9"));
}

// 8. Copy-out is UAF-safe: the caller's buffer holds an independent COPY, not a
//    pointer into the entry. After reading generation-1 into buf_a, a second
//    update + get_serialized re-serializes (freeing the entry's internal buffer)
//    — but buf_a must STILL hold the generation-1 bytes, proving an in-flight
//    reader cannot be corrupted by a concurrent sampler re-serialize.
void test_bb_pub_telemetry_fidelity_cache_get_serialized_is_a_copy(void)
{
    fid_reset();
    TEST_ASSERT_EQUAL_INT(0, bb_cache_register_ex("telem_fid_cache", NULL,
                                                  sizeof(fid_snap_t), fid_serialize,
                                                  BB_CACHE_FLAG_NONE));

    fid_snap_t s = { .value = 111 };
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update("telem_fid_cache", &s));

    char   buf_a[256];
    size_t la = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid_cache", buf_a, sizeof(buf_a), &la));
    TEST_ASSERT_NOT_NULL(strstr(buf_a, "111"));

    // New generation — re-serialize frees + replaces the entry's internal buffer.
    s.value = 222;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update("telem_fid_cache", &s));
    char   buf_b[256];
    size_t lb = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid_cache", buf_b, sizeof(buf_b), &lb));

    // buf_a is an independent copy: unchanged by the second generation.
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf_a, "111"),
        "caller buffer must hold an independent copy (UAF-safe)");
    TEST_ASSERT_NULL_MESSAGE(strstr(buf_a, "222"),
        "second generation must not bleed into the first reader's buffer");
    TEST_ASSERT_NOT_NULL(strstr(buf_b, "222"));
    // Distinct caller storage.
    TEST_ASSERT_TRUE(buf_a != buf_b);
}

// 9. Buffer too small → BB_ERR_NO_SPACE, caller buffer untouched (no truncation).
void test_bb_pub_telemetry_fidelity_cache_get_serialized_no_space(void)
{
    fid_reset();
    TEST_ASSERT_EQUAL_INT(0, bb_cache_register_ex("telem_fid_cache", NULL,
                                                  sizeof(fid_snap_t), fid_serialize,
                                                  BB_CACHE_FLAG_NONE));
    fid_snap_t s = { .value = 1234567 };
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update("telem_fid_cache", &s));

    char   tiny[4] = { 'Z', 'Z', 'Z', 'Z' };
    size_t l = 99;
    bb_err_t rc = bb_cache_get_serialized("telem_fid_cache", tiny, sizeof(tiny), &l);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_NO_SPACE, rc,
        "small buffer must return BB_ERR_NO_SPACE, not truncate");
    // Buffer left untouched on NO_SPACE.
    TEST_ASSERT_EQUAL_CHAR('Z', tiny[0]);
}
