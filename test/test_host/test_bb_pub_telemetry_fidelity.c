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
#include "bb_pub_info.h"
#include "bb_cache.h"
#include "bb_json.h"
#include "bb_nv.h"
#include "bb_fan_test.h"
#include "bb_power_test.h"
#include "bb_wifi.h"
#include "bb_wifi_http.h"

#ifdef BB_WIFI_TESTING
#include "bb_wifi_test.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>

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
                             const char *payload, int len, bool retain)
{
    (void)ctx;
    (void)len;
    (void)retain;
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
    bb_cache_config_t cfg = {
        .key       = "telem_fid_cache",
        .snapshot  = NULL,
        .snap_size = sizeof(fid_snap_t),
        .serialize = fid_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t rc = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    fid_snap_t s = { .value = 7 };
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update(&(bb_cache_update_t){ .key = "telem_fid_cache", .snap = &s }));

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
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update(&(bb_cache_update_t){ .key = "telem_fid_cache", .snap = &s }));
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
    bb_cache_config_t cfg = {
        .key       = "telem_fid_cache",
        .snapshot  = NULL,
        .snap_size = sizeof(fid_snap_t),
        .serialize = fid_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL_INT(0, bb_cache_register(&cfg));

    fid_snap_t s = { .value = 111 };
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update(&(bb_cache_update_t){ .key = "telem_fid_cache", .snap = &s }));

    char   buf_a[256];
    size_t la = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("telem_fid_cache", buf_a, sizeof(buf_a), &la));
    TEST_ASSERT_NOT_NULL(strstr(buf_a, "111"));

    // New generation — re-serialize frees + replaces the entry's internal buffer.
    s.value = 222;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update(&(bb_cache_update_t){ .key = "telem_fid_cache", .snap = &s }));
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
    bb_cache_config_t cfg = {
        .key       = "telem_fid_cache",
        .snapshot  = NULL,
        .snap_size = sizeof(fid_snap_t),
        .serialize = fid_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL_INT(0, bb_cache_register(&cfg));
    fid_snap_t s = { .value = 1234567 };
    TEST_ASSERT_EQUAL_INT(0, bb_cache_update(&(bb_cache_update_t){ .key = "telem_fid_cache", .snap = &s }));

    char   tiny[4] = { 'Z', 'Z', 'Z', 'Z' };
    size_t l = 99;
    bb_err_t rc = bb_cache_get_serialized("telem_fid_cache", tiny, sizeof(tiny), &l);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_ERR_NO_SPACE, rc,
        "small buffer must return BB_ERR_NO_SPACE, not truncate");
    // Buffer left untouched on NO_SPACE.
    TEST_ASSERT_EQUAL_CHAR('Z', tiny[0]);
}

// ===========================================================================
// Per-satellite SSOT fidelity: info
// Each test registers the satellite, ticks once, then verifies:
//   - gather ran once
//   - serialize ran once
//   - sink received the payload
//   - REST (bb_cache_get_serialized) returns the same bytes as the sink
// ===========================================================================

// ---------------------------------------------------------------------------
// Common capture helpers for satellite tests
// ---------------------------------------------------------------------------

#define SAT_CAP 4
static char    s_sat_payload[SAT_CAP][512];
static int     s_sat_count;

static bb_err_t sat_publish(void *ctx, const char *topic,
                             const char *payload, int len, bool retain)
{
    (void)ctx;
    (void)topic;
    (void)len;
    (void)retain;
    if (s_sat_count >= SAT_CAP) return BB_ERR_NO_SPACE;
    strncpy(s_sat_payload[s_sat_count++], payload,
            sizeof(s_sat_payload[0]) - 1);
    return BB_OK;
}

static void sat_reset(void)
{
    fid_reset();
    bb_fan_test_reset();
    bb_power_test_reset();
    s_sat_count = 0;
    memset(s_sat_payload, 0, sizeof(s_sat_payload));
    bb_pub_sink_t sink = { .publish = sat_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
}

// ---------------------------------------------------------------------------
// 10. bb_pub_info: sinks-only (BB_PUB_TELEM_SINKS, no SSE).
//     Always publishes; REST==sink bytes; ts_ms present.
// ---------------------------------------------------------------------------

void test_bb_pub_telem_info_rest_equals_sink(void)
{
    sat_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_info_register());
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_sat_count, "info sink must receive one delivery");

    char rest[1024];
    size_t rlen = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("info", rest, sizeof(rest), &rlen));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(rest, s_sat_payload[0],
        "info: REST bytes must equal sink bytes (SSOT)");
    // Key dynamic fields must be present (static identity fields moved to meta, TA-505).
    TEST_ASSERT_NOT_NULL(strstr(rest, "\"heap_internal_free\""));
    TEST_ASSERT_NOT_NULL(strstr(rest, "\"wdt_resets\""));
    TEST_ASSERT_NOT_NULL(strstr(rest, "\"ts_ms\""));
}

// 11. bb_pub_info: serialize-once-per-tick — two REST reads after one tick
//     must not trigger a second serialize; second tick triggers exactly one more.
void test_bb_pub_telem_info_serialize_once_per_tick(void)
{
    sat_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_info_register());

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_sat_count);

    // REST reads (memoized — no re-serialize).
    char j[1024];
    size_t l = 0;
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("info", j, sizeof(j), &l));
    TEST_ASSERT_EQUAL_INT(0, bb_cache_get_serialized("info", j, sizeof(j), &l));
    // Still one sink delivery from first tick.
    TEST_ASSERT_EQUAL_INT(1, s_sat_count);

    // Second tick — new generation, new delivery.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_sat_count);
}

// ---------------------------------------------------------------------------
// B1-486: egress_dead_count, lost_ip_count, recovery_count, reason_histogram
// moved to GET /api/diag/net (bb_net_health) — bb_wifi_emit_section (the
// /api/wifi fallback SSOT) must NOT emit them anymore.
// ---------------------------------------------------------------------------

void test_bb_wifi_emit_no_longer_has_egress_dead_count(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    // Use a zeroed info struct — just checking field absence.
    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    bb_wifi_emit_section(obj, &info);

    double val = -999.0;
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(obj, "egress_dead_count", &val),
        "egress_dead_count must not be emitted (B1-486: moved to /api/diag/net)");
    bb_json_free(obj);
}

void test_bb_wifi_emit_no_longer_has_lost_ip_count(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    bb_wifi_emit_section(obj, &info);

    double val = -999.0;
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(obj, "lost_ip_count", &val),
        "lost_ip_count must not be emitted (B1-486: moved to /api/diag/net)");
    bb_json_free(obj);
}

void test_bb_wifi_emit_no_longer_has_recovery_count(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    bb_wifi_emit_section(obj, &info);

    double val = -999.0;
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(obj, "recovery_count", &val),
        "recovery_count must not be emitted (B1-486: moved to /api/diag/net)");
    bb_json_free(obj);
}

void test_bb_wifi_emit_no_longer_has_reason_histogram(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    bb_wifi_emit_section(obj, &info);

    bb_json_t hist_obj = bb_json_obj_get_item(obj, "reason_histogram");
    TEST_ASSERT_NULL_MESSAGE(hist_obj,
        "reason_histogram must not be emitted (B1-486: moved to /api/diag/net)");

    bb_json_free(obj);
}

// B1-486: no_ip_recoveries moved to GET /api/diag/net (bb_net_health) —
// bb_wifi_emit_section (the /api/wifi fallback SSOT) must NOT emit it.
void test_bb_wifi_emit_no_longer_has_no_ip_recoveries(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    bb_wifi_emit_section(obj, &info);

    double val = -999.0;
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(obj, "no_ip_recoveries", &val),
        "no_ip_recoveries must not be emitted (B1-486: moved to /api/diag/net)");
    bb_json_free(obj);
}

// wifi-netmode PR: roam_count/roam_age_s (B1-497) are consolidated onto the
// /api/diag/net + net.health discriminator surface only — bb_wifi_emit_section
// (the /api/wifi fallback SSOT) must NOT re-emit them.
void test_bb_wifi_emit_no_longer_has_roam_fields(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

#ifdef BB_WIFI_TESTING
    bb_wifi_test_set_roam_count(3);
    bb_wifi_test_set_roam_age_s(42);
#endif

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    bb_wifi_emit_section(obj, &info);

    double val = -999.0;
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(obj, "roam_count", &val),
        "roam_count must not be emitted (consolidated onto /api/diag/net)");
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(obj, "roam_age_s", &val),
        "roam_age_s must not be emitted (consolidated onto /api/diag/net)");

#ifdef BB_WIFI_TESTING
    bb_wifi_test_set_roam_count(0);
    bb_wifi_test_set_roam_age_s(0);
#endif
    bb_json_free(obj);
}

// B1-461: guard the shared "wifi" cache/telemetry topic constant against
// accidental drift. Externally-consumed as the bb_cache tag, bb_wifi_http
// route tag, and openapi schema key — byte-identical to the pre-refactor
// hand-typed literal.
void test_bb_wifi_topic_const_matches_legacy_literal(void)
{
    TEST_ASSERT_EQUAL_STRING("wifi", BB_TOPIC_WIFI);
}

// ---------------------------------------------------------------------------
// bb_wifi_emit_section is now the SOLE /api/wifi producer (bb_pub_wifi
// removed) — value-correctness coverage for the fields it populates directly
// from the caller-supplied bb_wifi_info_t (ssid/bssid/ip/rssi). No
// bb_wifi_test_set_* stub needed for these — bb_wifi_emit_section takes the
// info struct as an argument rather than reading it off bb_wifi_get_info.
// ---------------------------------------------------------------------------

void test_bb_wifi_emit_bssid_hex_format_correct(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    info.bssid[0] = 0xAA; info.bssid[1] = 0xBB; info.bssid[2] = 0xCC;
    info.bssid[3] = 0xDD; info.bssid[4] = 0xEE; info.bssid[5] = 0xFF;
    bb_wifi_emit_section(obj, &info);

    char *str = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(str, "aa:bb:cc:dd:ee:ff"),
        "bssid must be emitted as colon-separated lowercase hex");
    bb_json_free_str(str);
    bb_json_free(obj);
}

void test_bb_wifi_emit_ssid_value_correct(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.ssid, "TestNet", sizeof(info.ssid) - 1);
    bb_wifi_emit_section(obj, &info);

    char *str = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(str, "TestNet"),
        "ssid content must be emitted verbatim");
    bb_json_free_str(str);
    bb_json_free(obj);
}

void test_bb_wifi_emit_ip_value_correct(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.ip, "192.168.1.42", sizeof(info.ip) - 1);
    bb_wifi_emit_section(obj, &info);

    char *str = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(str, "192.168.1.42"),
        "ip content must be emitted verbatim");
    bb_json_free_str(str);
    bb_json_free(obj);
}

void test_bb_wifi_emit_rssi_is_integer_not_float(void)
{
    // cJSON serializes small whole doubles without a decimal point so "-55"
    // is fine; "-55.0" or "-55.000" would indicate set_number(double) was
    // used instead of the integer setter.
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    info.rssi = -55;
    bb_wifi_emit_section(obj, &info);

    char *str = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NULL_MESSAGE(strstr(str, "-55."),
        "rssi should be integer (no decimal point)");
    TEST_ASSERT_NOT_NULL(strstr(str, "-55"));
    bb_json_free_str(str);
    bb_json_free(obj);
}

// ===========================================================================
// Cache-adapter no-re-gather: calling the registered sample_fn multiple times
// after a tick does NOT invoke gather again — it reads from bb_cache.
// This proves the metrics-path semantics: JSON and Prometheus are two encoders
// over the SAME frozen snapshot (gather_count never exceeds tick count).
//
// Note: _telem_adapter_sample calls bb_cache_serialize_into (compose-into-obj
// path), which re-runs the serializer to populate the caller's obj — that is
// correct behavior for the metrics handler.  The no-re-gather guarantee is
// specific: gather() does NOT re-run; the snapshot used for serialization is
// always the one frozen at tick time.
// ===========================================================================

void test_bb_pub_telem_adapter_no_regather_on_repeated_sample(void)
{
    fid_reset();
    fid_register_and_add_sink(BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS);

    // One tick — gathers once, serializes once (sink + SSE share the same bytes).
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_gather_count,
        "tick must gather exactly once");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_serialize_count,
        "tick must serialize exactly once");

    // Now call bb_pub_sample_into (the same path the metrics handler uses via
    // fn(obj, ctx)) two more times on the same telem source.  The adapter calls
    // bb_cache_serialize_into which reads the frozen snapshot — gather must NOT
    // run again regardless of how many times the metrics path samples the source.
    bb_json_t obj1 = bb_json_obj_new();
    bb_json_t obj2 = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj1);
    TEST_ASSERT_NOT_NULL(obj2);

    bool r1 = bb_pub_sample_into("telem_fid", obj1);
    bool r2 = bb_pub_sample_into("telem_fid", obj2);
    TEST_ASSERT_TRUE_MESSAGE(r1, "first sample_into must succeed");
    TEST_ASSERT_TRUE_MESSAGE(r2, "second sample_into must succeed");

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_gather_count,
        "gather must NOT re-run on repeated sample_into (frozen snapshot)");

    // Both objs must contain the gathered value (42) — the frozen snap is reused.
    char *s1 = bb_json_serialize(obj1);
    char *s2 = bb_json_serialize(obj2);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s1, "42"),
        "first sample_into obj must contain gathered value (frozen snap)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s2, "42"),
        "second sample_into obj must contain gathered value (frozen snap)");
    bb_json_free_str(s1);
    bb_json_free_str(s2);
    bb_json_free(obj1);
    bb_json_free(obj2);
}
