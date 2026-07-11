// Tests for bb_sink_ws: WebSocket sink adapter for bb_pub.
// Coverage target: 100% line/function/branch.
#include "unity.h"
#include "bb_sink_ws.h"
#include "bb_pub.h"
#include "bb_ws_server.h"
#include "bb_ws_server_host.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "test_hostname_seed.h"
#include "bb_cache.h"
#include "bb_cache_reactive.h"
#include "test_alloc_inject.h"

#include <string.h>
#include <stdlib.h>

// Test reset hook (BB_CACHE_TESTING).
void bb_cache_reset_for_test(void);

// ---------------------------------------------------------------------------
// Sample helpers
// ---------------------------------------------------------------------------

static bool sample_power(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "vout_mv", 1200.0);
    return true;
}

static bool sample_skip(bb_json_t obj, void *ctx)
{
    (void)obj;
    (void)ctx;
    return false;
}

// ---------------------------------------------------------------------------
// bb_cache test topic helpers (reactive delta + snapshot-on-connect tests)
// ---------------------------------------------------------------------------

typedef struct {
    int value;
} ws_cache_snap_t;

static void ws_cache_serialize(bb_json_t obj, const void *snap)
{
    const ws_cache_snap_t *s = (const ws_cache_snap_t *)snap;
    bb_json_obj_set_int(obj, "value", s->value);
}

static bb_err_t ws_cache_reg_owned(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(ws_cache_snap_t),
        .serialize = ws_cache_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

// Getter-mode key with no snapshot data available (getter always returns
// NULL) -- bb_cache_get_serialized returns BB_ERR_INVALID_STATE for it,
// exercising the "no snapshot available" skip branch in snapshot_key_to_fd.
static const void *ws_cache_getter_null(void) { return NULL; }

static bb_err_t ws_cache_reg_getter_null(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = ws_cache_getter_null,
        .snap_size = 0,
        .serialize = ws_cache_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

// ---------------------------------------------------------------------------
// Per-test reset helper
// ---------------------------------------------------------------------------

static void ws_sink_setup(void)
{
    bb_pub_test_reset();
    bb_ws_server_host_reset_captures();
    bb_sink_ws_reset_for_test();
    bb_test_seed_hostname("testhost");
    bb_cache_reset_for_test();
    bb_cache_reactive_reset_for_test();
    // The route registry is process-global and never reset by the shared
    // Unity setUp() (test_route_registry.c / test_bb_ws_server.c reset it
    // locally the same way). Every bb_sink_ws_init() call registers the
    // static "/ws" descriptor with no dedup, so without this the registry
    // (cap BB_ROUTE_REGISTRY_CAP=64) eventually overflows purely from
    // running enough bb_sink_ws tests in one process, independent of any
    // single test's correctness.
    bb_http_route_registry_clear();
}

// ---------------------------------------------------------------------------
// Subscription injection helper
//
// Injects a {"sub":[...]} TEXT frame for a specific fd so that
// subsequent broadcasts reach that client.  Must be called AFTER
// bb_sink_ws_init (which registers the handler) and AFTER the client
// fd is marked active.
// ---------------------------------------------------------------------------

static void inject_sub(bb_http_request_t *req, int fd, const char *sub_json)
{
    bb_ws_server_host_set_inject_fd(fd);
    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)sub_json,
        .len     = strlen(sub_json),
    };
    bb_ws_server_host_inject_frame(req, &frame);
    bb_ws_server_host_set_inject_fd(-1);
    // Flush any captured sends from the sub frame itself (none expected).
    bb_ws_server_host_async_reset();
}

// ---------------------------------------------------------------------------
// bb_sink_ws_init tests
// ---------------------------------------------------------------------------

void test_bb_sink_ws_init_null_out_returns_invalid_arg(void)
{
    ws_sink_setup();
    bb_err_t err = bb_sink_ws_init(NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_sink_ws_init_fills_sink(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    bb_err_t err = bb_sink_ws_init(NULL, &s);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(s.publish);
    TEST_ASSERT_EQUAL_STRING("websocket", s.transport);
    TEST_ASSERT_FALSE(s.tls);
    TEST_ASSERT_NULL(s.subscribe);
    TEST_ASSERT_NULL(s.subscribe_ctx);
}

// ---------------------------------------------------------------------------
// Publish: delivery via filtered broadcast
// ---------------------------------------------------------------------------

void test_bb_sink_ws_publish_broadcast_on_tick(void)
{
    ws_sink_setup();

    // Register one active WS client (fd=3) subscribed to telemetry.
    bb_ws_server_host_set_client_active(3, true);

    // Wire sink + source.
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("power", sample_power, NULL);

    // Inject subscription for fd=3.
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    // Should have one async broadcast to fd=3.
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());

    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(3, a->fd);
    TEST_ASSERT_EQUAL(BB_WS_TYPE_TEXT, a->type);
    TEST_ASSERT_TRUE(a->final);
    TEST_ASSERT_NOT_NULL(a->payload);

    // Envelope must contain ch and data fields.
    char msg[512];
    size_t copy_len = a->len < sizeof(msg) - 1 ? a->len : sizeof(msg) - 1;
    memcpy(msg, a->payload, copy_len);
    msg[copy_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"topic\":\"power\""));
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"data\":"));
    TEST_ASSERT_NOT_NULL(strstr(msg, "vout_mv"));
}

void test_bb_sink_ws_publish_skipped_source_not_broadcast(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("skip", sample_skip, NULL);

    // Subscribe fd=1 to telemetry — sample_skip still returns false.
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    // sample_skip returns false → bb_pub skips → nothing broadcast.
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_publish_no_clients_broadcast_all_ok(void)
{
    ws_sink_setup();
    // No clients active — broadcast returns BB_OK (no-op).

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("power", sample_power, NULL);

    bb_pub_tick_once();

    // No active fds → 0 async sends.
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_publish_multiple_sources(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(0, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("a", sample_power, NULL);
    bb_pub_register_source("b", sample_power, NULL);

    // Subscribe fd=0 to telemetry so both subtopics deliver.
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 0, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    // Two sources → two broadcasts.
    TEST_ASSERT_EQUAL_INT(2, bb_ws_server_host_async_count());

    const bb_ws_server_host_async_capture_t *a0 = bb_ws_server_host_async_at(0);
    const bb_ws_server_host_async_capture_t *a1 = bb_ws_server_host_async_at(1);
    TEST_ASSERT_NOT_NULL(a0);
    TEST_ASSERT_NOT_NULL(a1);

    char msg0[256], msg1[256];
    size_t l0 = a0->len < sizeof(msg0) - 1 ? a0->len : sizeof(msg0) - 1;
    size_t l1 = a1->len < sizeof(msg1) - 1 ? a1->len : sizeof(msg1) - 1;
    memcpy(msg0, a0->payload, l0); msg0[l0] = '\0';
    memcpy(msg1, a1->payload, l1); msg1[l1] = '\0';

    // Each must carry its subtopic in "ch".
    TEST_ASSERT_NOT_NULL(strstr(msg0, "\"topic\":\"a\""));
    TEST_ASSERT_NOT_NULL(strstr(msg1, "\"topic\":\"b\""));
}

void test_bb_sink_ws_publish_multiple_clients(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(2, true);
    bb_ws_server_host_set_client_active(5, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("power", sample_power, NULL);

    // Subscribe both fds to telemetry.
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 2, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");
    inject_sub(req, 5, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    // One source × two clients = two async sends.
    TEST_ASSERT_EQUAL_INT(2, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// Envelope format
// ---------------------------------------------------------------------------

void test_bb_sink_ws_envelope_format(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(0, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);
    bb_pub_set_sink(&s);
    bb_pub_register_source("telemetry", sample_power, NULL);

    // Subscribe fd=0 — "telemetry" group matches subtopic "telemetry" exactly.
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 0, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);

    // Must start with {"type":"push","topic":"telemetry","ts_ms": -- the
    // envelope's ts_ms/data are HOISTED to the frame level (B1-570 PR-3), not
    // nested under "data" (which would double-wrap: {"data":{"ts_ms","data"}}).
    char expected_prefix[] = "{\"type\":\"push\",\"topic\":\"telemetry\",\"ts_ms\":";
    TEST_ASSERT_EQUAL_INT(0,
        strncmp((const char *)a->payload, expected_prefix, strlen(expected_prefix)));
    // Must end with }
    TEST_ASSERT_EQUAL('}', ((const char *)a->payload)[a->len - 1]);

    // "type" field must be present and equal "push"; "data" must be present
    // and NOT itself contain a nested "ts_ms"/"data" pair (no double-wrap).
    char msg[256];
    size_t cl = a->len < sizeof(msg) - 1 ? a->len : sizeof(msg) - 1;
    memcpy(msg, a->payload, cl); msg[cl] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"type\":\"push\""));
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"data\":{"));
    TEST_ASSERT_NULL(strstr(msg, "\"data\":{\"ts_ms\""));
}

// ---------------------------------------------------------------------------
// Direct publish call (bypassing bb_pub)
// ---------------------------------------------------------------------------

void test_bb_sink_ws_direct_publish_returns_ok(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // Subscribe fd=1 to "power" exactly.
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"power\"]}");

    // Simulate what bb_pub does: topic has prefix/hostname/subtopic form.
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", "{\"ts_ms\":1,\"data\":{\"v\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);

    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(strstr((const char *)a->payload, "\"topic\":\"power\""));
}

void test_bb_sink_ws_direct_publish_no_slash_topic(void)
{
    // Topic with fewer than 2 slashes — subtopic extraction reaches end of string.
    ws_sink_setup();
    bb_ws_server_host_set_client_active(0, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // Subscribe fd=0 to "" (exact empty string match for the extracted subtopic).
    // No client matches "" under any coarse group — 0 sends expected.
    bb_err_t err = s.publish(s.ctx, "bare", "{\"ts_ms\":1,\"data\":{\"x\":1}}", 26, false);
    // Should return BB_OK (no clients subscribed → filtered broadcast is a no-op).
    TEST_ASSERT_EQUAL(BB_OK, err);
}

// Regression: a "data" payload whose string value contains a literal '}'
// must not desync bb_json_envelope_split's brace-balance scan (which would
// truncate/corrupt the object forwarded to WS clients).
void test_bb_sink_ws_publish_brace_in_string_broadcasts_intact_object(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"power\"]}");

    const char *payload = "{\"ts_ms\":1,\"data\":{\"msg\":\"a}b\"}}";
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL(BB_OK, err);

    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    char msg[256];
    size_t cl = a->len < sizeof(msg) - 1 ? a->len : sizeof(msg) - 1;
    memcpy(msg, a->payload, cl); msg[cl] = '\0';
    // The "data" object must be forwarded intact, including the brace
    // embedded inside its string value -- not truncated at "a".
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"data\":{\"msg\":\"a}b\"}"));
}

// ---------------------------------------------------------------------------
// Envelope guard — sink_ws_publish rejects a payload that does not carry a
// well-formed {"ts_ms":<n>,"data":{...}} envelope (bb_json_envelope_split
// returns false). Covers every false-return branch of the shared helper.
// ---------------------------------------------------------------------------

void test_bb_sink_ws_publish_missing_envelope_returns_invalid_arg(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // No "ts_ms" or "data" keys at all.
    const char *payload = "{\"v\":1}";
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_publish_non_numeric_ts_ms_returns_invalid_arg(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // "ts_ms" value is a quoted string, not a bare integer literal.
    const char *payload = "{\"ts_ms\":\"nope\",\"data\":{\"v\":1}}";
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_publish_unbalanced_data_braces_returns_invalid_arg(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // "data" opens an object but the payload is truncated before the
    // matching close brace is ever reached.
    const char *payload = "{\"ts_ms\":1,\"data\":{\"v\":1";
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// bb_sink_ws_reset_for_test
// ---------------------------------------------------------------------------

void test_bb_sink_ws_reset_clears_state(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);
    bb_sink_ws_reset_for_test();

    // After reset, a fresh init must still work.
    bb_pub_sink_t s2;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s2));
    TEST_ASSERT_NOT_NULL(s2.publish);
}

// ---------------------------------------------------------------------------
// Handler registration tests
// ---------------------------------------------------------------------------

void test_bb_sink_ws_init_registers_ws_endpoint(void)
{
    // bb_sink_ws_init must register a /ws handler. Verify by injecting a frame
    // and confirming the handler returns BB_OK (not BB_ERR_INVALID_STATE which
    // inject_frame would return if no handler is registered).
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)"ping",
        .len     = 4,
    };
    bb_err_t err = bb_ws_server_host_inject_frame(req, &frame);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_sink_ws_init_register_fail_returns_error(void)
{
    ws_sink_setup();
    bb_ws_server_host_force_register_fail(true);
    bb_pub_sink_t s;
    bb_err_t err = bb_sink_ws_init(NULL, &s);
    bb_ws_server_host_force_register_fail(false);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_bb_sink_ws_handler_accepts_text_frame(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)"hello",
        .len     = 5,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_host_inject_frame(req, &frame));
}

void test_bb_sink_ws_handler_accepts_binary_frame(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t data[] = {0x01, 0x02, 0x03};
    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_BINARY,
        .payload = data,
        .len     = sizeof(data),
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_host_inject_frame(req, &frame));
}

// ---------------------------------------------------------------------------
// Subscription filtering tests (Piece 1)
// ---------------------------------------------------------------------------

// (a) client subscribed to ["telemetry"] receives telemetry frames but NOT
//     events or logs frames.
void test_bb_sink_ws_sub_telemetry_receives_telemetry_not_events_logs(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(4, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 4, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Publish a telemetry subtopic: should arrive.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());

    char msg[256];
    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    size_t cl = a->len < sizeof(msg) - 1 ? a->len : sizeof(msg) - 1;
    memcpy(msg, a->payload, cl); msg[cl] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"topic\":\"mining\""));

    bb_ws_server_host_async_reset();

    // Publish an events subtopic: should NOT arrive.
    err = s.publish(s.ctx, "m/h/events", "{\"ts_ms\":1,\"data\":{\"e\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());

    // Inject a structured log event: should NOT arrive for telemetry subscriber.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// (b) client subscribed to ["log"] receives structured log event.
void test_bb_sink_ws_sub_log_receives_log_event(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(6, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 6, "{\"type\":\"sub\",\"topic\":[\"log\"]}");

    bb_sink_ws_host_inject_log_event("{\"ts\":123,\"level\":\"I\",\"tag\":\"foo\",\"msg\":\"bar\"}");
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());

    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(6, a->fd);
    TEST_ASSERT_EQUAL(BB_WS_TYPE_TEXT, a->type);

    char msg[256];
    size_t cl = a->len < sizeof(msg) - 1 ? a->len : sizeof(msg) - 1;
    memcpy(msg, a->payload, cl); msg[cl] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"topic\":\"log\""));
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"data\":"));
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"ts\":123"));
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"msg\":\"bar\""));
}

// (b2) client NOT subscribed to "log" receives nothing from inject_log_event.
void test_bb_sink_ws_sub_log_unsubscribed_receives_nothing(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(7, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 7, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// (b3) suspended state blocks log event broadcast.
void test_bb_sink_ws_sub_log_suspend_gates(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(5, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 5, "{\"type\":\"sub\",\"topic\":[\"log\"]}");

    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());

    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// (b4) inject_log_event with no active clients is a no-op (no crash).
void test_bb_sink_ws_log_event_inject_no_clients(void)
{
    ws_sink_setup();

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    // No clients active — should complete silently.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// (b5) inject_log_event malloc fail → no broadcast, no crash.
void test_bb_sink_ws_log_event_inject_malloc_fail(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(9, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 9, "{\"type\":\"sub\",\"topic\":[\"log\"]}");

    bb_sink_ws_set_malloc(test_failing_malloc);
    test_alloc_fail_at = 0;
    test_alloc_reset();

    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());

    bb_sink_ws_set_malloc(NULL);
    test_alloc_fail_at = -1;
}

// (c) unsubscribed client (never sent a sub frame) receives nothing.
void test_bb_sink_ws_unsubscribed_client_receives_nothing(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(7, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("info", sample_power, NULL);

    // No sub frame injected for fd=7.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());

    // Log event inject also delivers nothing to unsubscribed client.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// Alloc-failure paths (host-reachable)
// ---------------------------------------------------------------------------

// publish malloc fail → returns BB_ERR_NO_SPACE, nothing broadcast.
void test_bb_sink_ws_publish_malloc_fail_returns_no_space(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(0, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 0, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Inject a failing malloc so sink_ws_publish returns BB_ERR_NO_SPACE.
    bb_sink_ws_set_malloc(test_failing_malloc);
    test_alloc_fail_at = 0;
    test_alloc_reset();

    bb_err_t err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());

    // Restore normal allocator.
    bb_sink_ws_set_malloc(NULL);
    test_alloc_fail_at = -1;
}

// ---------------------------------------------------------------------------
// Suspend / resume tests
// ---------------------------------------------------------------------------

// suspend with active clients clears sub tables; subsequent broadcast delivers to none.
void test_sink_ws_suspend_clears_clients(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(3, true);
    bb_ws_server_host_set_client_active(5, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");
    inject_sub(req, 5, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Pre-suspend: both clients receive.
    bb_err_t err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(2, bb_ws_server_host_async_count());
    bb_ws_server_host_async_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());

    // Post-suspend: no client receives.
    err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":2}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// disconnect notification clears subscription state for that fd — an fd
// reused by a new connection (LWIP fd reuse) never inherits the prior
// client's subscription filter (HIGH #2 fix: previously only suspend cleared
// subs, not a plain socket disconnect).
void test_sink_ws_disconnect_clears_client_sub_state(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Pre-disconnect: fd=3 receives.
    bb_err_t err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    bb_ws_server_host_async_reset();

    // Simulate the socket disconnecting (fd=3 goes away) without a suspend.
    bb_ws_server_host_simulate_disconnect(3);
    bb_ws_server_host_set_client_active(3, false);

    // A new connection reuses fd=3 (LWIP fd reuse) but never re-subscribes —
    // it must NOT inherit the old subscription.
    bb_ws_server_host_set_client_active(3, true);
    err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":2}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// suspend is idempotent: second call returns BB_OK without crashing.
void test_sink_ws_suspend_idempotent(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());
}

// resume clears the suspended flag; broadcasts deliver again after re-subscribe.
void test_sink_ws_resume_clears_flag(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(2, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());
    bb_sink_ws_resume();

    // Re-inject subscription (suspend cleared sub table).
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 2, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_err_t err = s.publish(s.ctx, "m/h/info", "{\"ts_ms\":1,\"data\":{\"x\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
}

// (d) malformed sub frame is ignored safely — state for the fd unchanged.
void test_bb_sink_ws_malformed_sub_frame_ignored(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(8, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    // Inject various malformed payloads — none should crash.
    const char *bad[] = {
        "not json",
        "{}",
        "{\"sub\":}",
        "{\"other\":[\"foo\"]}",
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bb_ws_server_host_set_inject_fd(8);
        bb_ws_server_frame_t frame = {
            .final   = true,
            .type    = BB_WS_TYPE_TEXT,
            .payload = (uint8_t *)bad[i],
            .len     = strlen(bad[i]),
        };
        bb_err_t err = bb_ws_server_host_inject_frame(req, &frame);
        TEST_ASSERT_EQUAL(BB_OK, err);
        bb_ws_server_host_set_inject_fd(-1);
    }
    bb_ws_server_host_async_reset();

    // fd=8 still has no valid subscriptions → publish delivers nothing.
    bb_pub_sink_t s2;
    bb_sink_ws_init(NULL, &s2);
    bb_err_t err = s2.publish(s2.ctx, "m/h/info", "{\"ts_ms\":1,\"data\":{\"x\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// Subscription: exact subtopic match (not just coarse group)
// ---------------------------------------------------------------------------

void test_bb_sink_ws_sub_exact_subtopic_match(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(0, true);
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    // fd=0 subscribed to exact "mining" subtopic only.
    inject_sub(req, 0, "{\"type\":\"sub\",\"topic\":[\"mining\"]}");
    // fd=1 subscribed to "pool" only.
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"pool\"]}");

    // Publish "mining": only fd=0 should receive.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    TEST_ASSERT_EQUAL(0, bb_ws_server_host_async_at(0)->fd);
    bb_ws_server_host_async_reset();

    // Publish "pool": only fd=1 should receive.
    err = s.publish(s.ctx, "m/h/pool", "{\"ts_ms\":1,\"data\":{\"p\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    TEST_ASSERT_EQUAL(1, bb_ws_server_host_async_at(0)->fd);
}

// ---------------------------------------------------------------------------
// Subscription: whitespace-tolerant envelope parse
// ---------------------------------------------------------------------------

// A pretty-printed sub frame (e.g. Python's json.dumps default ": "/", "
// separators, plus embedded newlines/indentation) must subscribe the client
// exactly like the compact form.
void test_bb_sink_ws_sub_whitespace_tolerant_parse(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(4, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 4,
        "{\n"
        "  \"type\": \"sub\",\n"
        "  \"topic\": [\n"
        "    \"telemetry\",\n"
        "    \"log\"\n"
        "  ]\n"
        "}");

    // Telemetry subtopic should arrive via the "telemetry" coarse group.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    bb_ws_server_host_async_reset();

    // "log" exact opt-in should also arrive.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// Subscription: coarse "events" group
// ---------------------------------------------------------------------------

void test_bb_sink_ws_sub_events_group(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(2, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 2, "{\"type\":\"sub\",\"topic\":[\"events\"]}");

    // "events" subtopic should arrive.
    bb_err_t err = s.publish(s.ctx, "m/h/events", "{\"ts_ms\":1,\"data\":{\"type\":\"share\"}}", 35, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    bb_ws_server_host_async_reset();

    // "mining" subtopic should NOT arrive.
    err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// Subscription: replace (re-sub overwrites previous)
// ---------------------------------------------------------------------------

void test_bb_sink_ws_sub_replace_on_resub(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    // First sub: telemetry.
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Confirm telemetry delivers.
    bb_err_t err = s.publish(s.ctx, "m/h/info", "{\"ts_ms\":1,\"data\":{}}", 21, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    bb_ws_server_host_async_reset();

    // Re-sub to log only — telemetry should no longer deliver.
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"log\"]}");

    err = s.publish(s.ctx, "m/h/info", "{\"ts_ms\":1,\"data\":{}}", 21, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());

    // But log events should now deliver.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"after resub\"}");
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// Inbound envelope demux: legacy back-compat, reserved types, pool sizing
// ---------------------------------------------------------------------------

// Legacy {"sub":[...]} (no "type" envelope) is still accepted.
void test_bb_sink_ws_legacy_sub_frame_back_compat(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(2, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // Legacy frame, no "type" key.
    inject_sub(req, 2, "{\"sub\":[\"telemetry\"]}");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
}

// An envelope with a "type" other than "sub" (e.g. "cmd") is RESERVED —
// ignored-with-log, no subscription change, no crash.
void test_bb_sink_ws_reserved_type_ignored(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"cmd\",\"topic\":[\"telemetry\"]}");

    // No subscription was created for fd=3 — publish delivers nothing.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// An envelope whose "type" value is not a quoted string (e.g. a bare number)
// fails extract_type's quote check -- treated as "no type key", falling
// through to the legacy {"sub":[...]} back-compat path (which also finds
// nothing here, since this frame has no "sub" key either).
void test_bb_sink_ws_type_value_not_quoted_treated_as_no_type(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":5,\"topic\":[\"telemetry\"]}");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// BB_SINK_WS_MAX_CLIENTS (right-sized to CONFIG_BB_HTTP_MAX_OPEN_SOCKETS,
// C default 5 on host) bounds the fd->slot map. A subscription beyond the
// cap is dropped (logged), never a crash or out-of-bounds write; earlier
// clients keep working normally.
void test_bb_sink_ws_client_pool_exhaustion_drops_extra_sub(void)
{
    ws_sink_setup();
    const int fds[] = {10, 20, 30, 40, 50, 60}; // 6 clients > default cap of 5
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        bb_ws_server_host_set_client_active(fds[i], true);
    }

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        inject_sub(req, fds[i], "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");
    }

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    // Only the first 5 (pool capacity) actually got a subscription slot.
    TEST_ASSERT_EQUAL_INT(5, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// bb_cache_reactive change-driven deltas (B1-589 PR-4b)
// ---------------------------------------------------------------------------

void test_bb_sink_ws_reactive_delta_broadcasts_on_cache_change(void)
{
    ws_sink_setup();
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.a"));
    bb_ws_server_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    ws_cache_snap_t snap = { .value = 1 };
    bb_cache_update_t update = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&update));

    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(3, a->fd);

    char msg[256];
    size_t copy_len = a->len < sizeof(msg) - 1 ? a->len : sizeof(msg) - 1;
    memcpy(msg, a->payload, copy_len);
    msg[copy_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"topic\":\"rx.a\""));
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"value\":1"));
}

void test_bb_sink_ws_reactive_delta_unchanged_rewrite_not_broadcast(void)
{
    ws_sink_setup();
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.a"));
    bb_ws_server_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    ws_cache_snap_t snap = { .value = 1 };
    bb_cache_update_t update = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&update));
    bb_ws_server_host_async_reset();

    // Identical rewrite: no change -> no additional delta.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&update));
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_reactive_delta_suspended_no_broadcast(void)
{
    ws_sink_setup();
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.a"));
    bb_ws_server_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());
    bb_ws_server_host_async_reset();

    ws_cache_snap_t snap = { .value = 1 };
    bb_cache_update_t update = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&update));
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_reactive_delta_malloc_fail_no_broadcast(void)
{
    ws_sink_setup();
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.a"));
    bb_ws_server_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_sink_ws_set_malloc(test_failing_malloc);
    test_alloc_fail_at = 0;
    ws_cache_snap_t snap = { .value = 1 };
    bb_cache_update_t update = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&update));
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
    bb_sink_ws_set_malloc(NULL);
    test_alloc_fail_at = -1;
}

// ---------------------------------------------------------------------------
// Snapshot-on-connect (B1-589 PR-4b)
// ---------------------------------------------------------------------------

void test_bb_sink_ws_snapshot_on_connect_sends_current_state(void)
{
    ws_sink_setup();
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.a"));
    ws_cache_snap_t snap = { .value = 42 };
    bb_cache_update_t update = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&update));

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_ws_server_host_simulate_connect(NULL, 7);

    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(7, a->fd); // unicast to the connecting fd only

    char msg[256];
    size_t copy_len = a->len < sizeof(msg) - 1 ? a->len : sizeof(msg) - 1;
    memcpy(msg, a->payload, copy_len);
    msg[copy_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"topic\":\"rx.a\""));
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"value\":42"));
}

void test_bb_sink_ws_snapshot_on_connect_multiple_keys_all_sent(void)
{
    ws_sink_setup();
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.a"));
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.b"));
    ws_cache_snap_t snap = { .value = 1 };
    bb_cache_update_t ua = { .key = "rx.a", .snap = &snap };
    bb_cache_update_t ub = { .key = "rx.b", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&ua));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&ub));

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_ws_server_host_simulate_connect(NULL, 9);

    TEST_ASSERT_EQUAL_INT(2, bb_ws_server_host_async_count());
    const bb_ws_server_host_async_capture_t *a0 = bb_ws_server_host_async_at(0);
    const bb_ws_server_host_async_capture_t *a1 = bb_ws_server_host_async_at(1);
    TEST_ASSERT_EQUAL(9, a0->fd);
    TEST_ASSERT_EQUAL(9, a1->fd);
}

void test_bb_sink_ws_snapshot_on_connect_no_keys_no_captures(void)
{
    ws_sink_setup();

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_ws_server_host_simulate_connect(NULL, 7);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_snapshot_on_connect_suspended_skips(void)
{
    ws_sink_setup();
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.a"));
    ws_cache_snap_t snap = { .value = 1 };
    bb_cache_update_t update = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&update));

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());
    bb_ws_server_host_async_reset();

    bb_ws_server_host_simulate_connect(NULL, 7);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_snapshot_on_connect_get_serialized_failure_skipped(void)
{
    ws_sink_setup();
    // Getter-mode key whose getter always returns NULL -> get_serialized
    // returns BB_ERR_INVALID_STATE; snapshot must skip it without crashing.
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_getter_null("rx.empty"));

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_ws_server_host_simulate_connect(NULL, 7);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_snapshot_on_connect_malloc_fail_skipped(void)
{
    ws_sink_setup();
    TEST_ASSERT_EQUAL(BB_OK, ws_cache_reg_owned("rx.a"));
    ws_cache_snap_t snap = { .value = 1 };
    bb_cache_update_t update = { .key = "rx.a", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&update));

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_sink_ws_set_malloc(test_failing_malloc);
    test_alloc_fail_at = 0;
    bb_ws_server_host_simulate_connect(NULL, 7);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
    bb_sink_ws_set_malloc(NULL);
    test_alloc_fail_at = -1;
}

// ---------------------------------------------------------------------------
// B1-516 pass 2: coverage-closing tests (bb_sink_ws) — no production seam
// added; all use existing host-stub hooks (inject_fd, force_async_alloc_fail).
// ---------------------------------------------------------------------------

// client_slot_acquire: fd<0 short-circuit guard (via parse_topic_array).
void test_bb_sink_ws_dispatch_negative_fd_is_ignored(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // Must not crash; fd=-1 can never own a slot.
    inject_sub(req, -1, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// client_slot_find's OWN fd<0 guard is reached via client_sub_clear (the
// disconnect path), which calls client_slot_find directly rather than via
// client_slot_acquire's own separate fd<0 check.
void test_bb_sink_ws_disconnect_negative_fd_is_ignored(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    // Must not crash.
    bb_ws_server_host_simulate_disconnect(-1);
}

// parse_topic_array: unterminated string (no closing quote before frame end)
// must fail safely (no crash / OOB read) rather than parse a partial topic.
void test_bb_sink_ws_sub_topic_unterminated_string_ignored(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // Opening quote for the topic string is never closed.
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"telemetry");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// parse_topic_array: more entries than BB_SINK_WS_MAX_SUBS (8) — extras are
// dropped (loop cutoff), the first MAX_SUBS entries are kept.
void test_bb_sink_ws_sub_topic_array_overflow_caps_at_max_subs(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // 9 distinct exact-match subtopics — one more than BB_SINK_WS_MAX_SUBS.
    inject_sub(req, 1,
        "{\"type\":\"sub\",\"topic\":["
        "\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}");

    // The first entry ("a") must still be subscribed.
    bb_err_t err = s.publish(s.ctx, "m/h/a", "{\"ts_ms\":1,\"data\":{}}", 21, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
}

// parse_topic_array: a topic name longer than BB_SINK_WS_SUB_MAX_LEN (32)
// is truncated to fit rather than overflowing the fixed-size slot.
void test_bb_sink_ws_sub_topic_name_truncated_to_fit(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // 40 'x' chars — longer than BB_SINK_WS_SUB_MAX_LEN (32).
    inject_sub(req, 1,
        "{\"type\":\"sub\",\"topic\":"
        "[\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"]}");

    // Must not crash; a subtopic matching the truncated string still won't
    // exact-match a distinct real subtopic, so nothing should be delivered.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// find_array_start: the "topic" key is present but its value is NOT an
// array (e.g. a bare string) — must be rejected, not mis-parsed.
void test_bb_sink_ws_sub_topic_value_not_array_ignored(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":\"telemetry\"}");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// Legacy back-compat: the "sub" key present but not an array is rejected.
void test_bb_sink_ws_legacy_sub_value_not_array_ignored(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"sub\":\"telemetry\"}");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// extract_type: an unterminated "type" string value (no closing quote
// before the frame ends) must fail safely, falling through to legacy parse.
void test_bb_sink_ws_type_value_unterminated_falls_back_to_legacy(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // "type" value's closing quote is missing — extract_type must return
    // false (falls through to the legacy {"sub":[...]} path), which also
    // finds no "sub" key here, so the frame is ignored.
    inject_sub(req, 1, "{\"type\":\"sub");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// extract_type: a "type" value longer than BB_SINK_WS_TYPE_MAX_LEN (16) is
// truncated to fit rather than overflowing type_buf; it's still correctly
// recognised as not "sub" (RESERVED, ignored-with-log).
void test_bb_sink_ws_type_value_overlong_truncated_and_reserved(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // 20 chars — longer than BB_SINK_WS_TYPE_MAX_LEN (16).
    inject_sub(req, 1, "{\"type\":\"aaaaaaaaaaaaaaaaaaaa\",\"topic\":[\"telemetry\"]}");

    // No subscription created (type wasn't "sub") — publish delivers nothing.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// broadcast_filtered: bb_ws_server_broadcast_frame_async failing for a
// subscribed client must propagate the error from publish() without
// crashing or stopping delivery bookkeeping for other clients.
void test_bb_sink_ws_publish_broadcast_async_failure_propagates(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);
    bb_ws_server_host_set_client_active(2, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");
    inject_sub(req, 2, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_ws_server_host_force_async_alloc_fail(true);
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    bb_ws_server_host_force_async_alloc_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

// parse_topic_array: array runs to end-of-buffer immediately after a valid
// closing quote, with no closing ']' — the outer/inner scan loops must
// terminate on r==end rather than reading past it.
void test_bb_sink_ws_sub_topic_array_unclosed_at_buffer_end(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // Buffer ends exactly after the closing quote of "a" — no ']', no comma,
    // nothing else follows.
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"a\"");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// parse_topic_array: after a comma, trailing garbage with no opening quote
// and no ']' runs the inner "scan for quote" loop off the end of the buffer.
void test_bb_sink_ws_sub_topic_array_trailing_garbage_at_buffer_end(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"a\",xyz");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// find_key_value: whitespace-tolerant skip must handle a lone tab and a
// lone carriage return between the colon and the value, not just space/LF
// (already covered by the pretty-printed-JSON test).
void test_bb_sink_ws_sub_whitespace_tab_and_cr_tolerant(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\t\"sub\",\"topic\":\r[\"telemetry\"]}");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
}

// find_key_value: a lone newline directly after the colon (distinct from
// the multi-space pretty-printed-JSON indentation case above).
void test_bb_sink_ws_sub_whitespace_newline_directly_after_colon(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\n\"sub\",\"topic\":[\"telemetry\"]}");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_ws_server_host_async_count());
}

// find_array_start / extract_type: the sought key is the very last thing in
// the payload (colon with nothing after it) — the "found key, but v>=end"
// guard must reject it rather than reading past the buffer.
void test_bb_sink_ws_sub_key_at_buffer_end_no_value(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // "topic": is the last thing in the buffer -- nothing follows the colon.
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

void test_bb_sink_ws_type_key_at_buffer_end_no_value(void)
{
    ws_sink_setup();
    bb_ws_server_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    // "type": is the last thing in the buffer -- extract_type must reject
    // it (v>=end) and fall through to the legacy {"sub":[...]} parse, which
    // also finds nothing, so the frame is ignored.
    inject_sub(req, 1, "{\"type\":");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_ws_server_host_async_count());
}

// ws_handler: a TEXT frame with a NULL payload (len>0) must be rejected by
// the guard clause without dereferencing payload. Constructed directly
// (not via inject_sub) since bb_ws_server_host_inject_frame() forwards the
// frame pointer straight to the registered handler.
void test_bb_sink_ws_handler_null_payload_text_frame_ignored(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    bb_ws_server_host_set_inject_fd(1);
    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = NULL,
        .len     = 10,
    };
    bb_err_t err = bb_ws_server_host_inject_frame(req, &frame);
    bb_ws_server_host_set_inject_fd(-1);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

// bb_sink_ws_host_inject_log_event(NULL) must be a safe no-op.
void test_bb_sink_ws_host_inject_log_event_null_json_is_noop(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    // Must not crash.
    bb_sink_ws_host_inject_log_event(NULL);
}
