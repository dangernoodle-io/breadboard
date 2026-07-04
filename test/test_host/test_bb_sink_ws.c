// Tests for bb_sink_ws: WebSocket sink adapter for bb_pub.
// Coverage target: 100% line/function/branch.
#include "unity.h"
#include "bb_sink_ws.h"
#include "bb_pub.h"
#include "bb_websocket.h"
#include "bb_websocket_host.h"
#include "bb_nv.h"
#include "test_alloc_inject.h"

#include <string.h>
#include <stdlib.h>

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
// Per-test reset helper
// ---------------------------------------------------------------------------

static void ws_sink_setup(void)
{
    bb_pub_test_reset();
    bb_websocket_host_reset_captures();
    bb_sink_ws_reset_for_test();
    bb_nv_config_set_hostname("testhost");
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
    bb_websocket_host_set_inject_fd(fd);
    bb_websocket_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)sub_json,
        .len     = strlen(sub_json),
    };
    bb_websocket_host_inject_frame(req, &frame);
    bb_websocket_host_set_inject_fd(-1);
    // Flush any captured sends from the sub frame itself (none expected).
    bb_websocket_host_async_reset();
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
    bb_websocket_host_set_client_active(3, true);

    // Wire sink + source.
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("power", sample_power, NULL);

    // Inject subscription for fd=3.
    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    // Should have one async broadcast to fd=3.
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());

    const bb_websocket_host_async_capture_t *a = bb_websocket_host_async_at(0);
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
    bb_websocket_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("skip", sample_skip, NULL);

    // Subscribe fd=1 to telemetry — sample_skip still returns false.
    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    // sample_skip returns false → bb_pub skips → nothing broadcast.
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
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
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

void test_bb_sink_ws_publish_multiple_sources(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(0, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("a", sample_power, NULL);
    bb_pub_register_source("b", sample_power, NULL);

    // Subscribe fd=0 to telemetry so both subtopics deliver.
    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 0, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    // Two sources → two broadcasts.
    TEST_ASSERT_EQUAL_INT(2, bb_websocket_host_async_count());

    const bb_websocket_host_async_capture_t *a0 = bb_websocket_host_async_at(0);
    const bb_websocket_host_async_capture_t *a1 = bb_websocket_host_async_at(1);
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
    bb_websocket_host_set_client_active(2, true);
    bb_websocket_host_set_client_active(5, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("power", sample_power, NULL);

    // Subscribe both fds to telemetry.
    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 2, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");
    inject_sub(req, 5, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    // One source × two clients = two async sends.
    TEST_ASSERT_EQUAL_INT(2, bb_websocket_host_async_count());
}

// ---------------------------------------------------------------------------
// Envelope format
// ---------------------------------------------------------------------------

void test_bb_sink_ws_envelope_format(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(0, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);
    bb_pub_set_sink(&s);
    bb_pub_register_source("telemetry", sample_power, NULL);

    // Subscribe fd=0 — "telemetry" group matches subtopic "telemetry" exactly.
    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 0, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_pub_tick_once();

    const bb_websocket_host_async_capture_t *a = bb_websocket_host_async_at(0);
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
    bb_websocket_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // Subscribe fd=1 to "power" exactly.
    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"power\"]}");

    // Simulate what bb_pub does: topic has prefix/hostname/subtopic form.
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", "{\"ts_ms\":1,\"data\":{\"v\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);

    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
    const bb_websocket_host_async_capture_t *a = bb_websocket_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(strstr((const char *)a->payload, "\"topic\":\"power\""));
}

void test_bb_sink_ws_direct_publish_no_slash_topic(void)
{
    // Topic with fewer than 2 slashes — subtopic extraction reaches end of string.
    ws_sink_setup();
    bb_websocket_host_set_client_active(0, true);

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
    bb_websocket_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"power\"]}");

    const char *payload = "{\"ts_ms\":1,\"data\":{\"msg\":\"a}b\"}}";
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL(BB_OK, err);

    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
    const bb_websocket_host_async_capture_t *a = bb_websocket_host_async_at(0);
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
    bb_websocket_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // No "ts_ms" or "data" keys at all.
    const char *payload = "{\"v\":1}";
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

void test_bb_sink_ws_publish_non_numeric_ts_ms_returns_invalid_arg(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // "ts_ms" value is a quoted string, not a bare integer literal.
    const char *payload = "{\"ts_ms\":\"nope\",\"data\":{\"v\":1}}";
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

void test_bb_sink_ws_publish_unbalanced_data_braces_returns_invalid_arg(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(1, true);

    bb_pub_sink_t s;
    bb_sink_ws_init(NULL, &s);

    // "data" opens an object but the payload is truncated before the
    // matching close brace is ever reached.
    const char *payload = "{\"ts_ms\":1,\"data\":{\"v\":1";
    bb_err_t err = s.publish(s.ctx, "metrics/testhost/power", payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
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
    bb_websocket_host_capture_begin(&req);
    bb_websocket_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)"ping",
        .len     = 4,
    };
    bb_err_t err = bb_websocket_host_inject_frame(req, &frame);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_sink_ws_init_register_fail_returns_error(void)
{
    ws_sink_setup();
    bb_websocket_host_force_register_fail(true);
    bb_pub_sink_t s;
    bb_err_t err = bb_sink_ws_init(NULL, &s);
    bb_websocket_host_force_register_fail(false);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_bb_sink_ws_handler_accepts_text_frame(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    bb_websocket_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)"hello",
        .len     = 5,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_websocket_host_inject_frame(req, &frame));
}

void test_bb_sink_ws_handler_accepts_binary_frame(void)
{
    ws_sink_setup();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    uint8_t data[] = {0x01, 0x02, 0x03};
    bb_websocket_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_BINARY,
        .payload = data,
        .len     = sizeof(data),
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_websocket_host_inject_frame(req, &frame));
}

// ---------------------------------------------------------------------------
// Subscription filtering tests (Piece 1)
// ---------------------------------------------------------------------------

// (a) client subscribed to ["telemetry"] receives telemetry frames but NOT
//     events or logs frames.
void test_bb_sink_ws_sub_telemetry_receives_telemetry_not_events_logs(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(4, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 4, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Publish a telemetry subtopic: should arrive.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());

    char msg[256];
    const bb_websocket_host_async_capture_t *a = bb_websocket_host_async_at(0);
    size_t cl = a->len < sizeof(msg) - 1 ? a->len : sizeof(msg) - 1;
    memcpy(msg, a->payload, cl); msg[cl] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(msg, "\"topic\":\"mining\""));

    bb_websocket_host_async_reset();

    // Publish an events subtopic: should NOT arrive.
    err = s.publish(s.ctx, "m/h/events", "{\"ts_ms\":1,\"data\":{\"e\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());

    // Inject a structured log event: should NOT arrive for telemetry subscriber.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

// (b) client subscribed to ["log"] receives structured log event.
void test_bb_sink_ws_sub_log_receives_log_event(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(6, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 6, "{\"type\":\"sub\",\"topic\":[\"log\"]}");

    bb_sink_ws_host_inject_log_event("{\"ts\":123,\"level\":\"I\",\"tag\":\"foo\",\"msg\":\"bar\"}");
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());

    const bb_websocket_host_async_capture_t *a = bb_websocket_host_async_at(0);
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
    bb_websocket_host_set_client_active(7, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 7, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

// (b3) suspended state blocks log event broadcast.
void test_bb_sink_ws_sub_log_suspend_gates(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(5, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 5, "{\"type\":\"sub\",\"topic\":[\"log\"]}");

    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());

    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

// (b4) inject_log_event with no active clients is a no-op (no crash).
void test_bb_sink_ws_log_event_inject_no_clients(void)
{
    ws_sink_setup();

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    // No clients active — should complete silently.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

// (b5) inject_log_event malloc fail → no broadcast, no crash.
void test_bb_sink_ws_log_event_inject_malloc_fail(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(9, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 9, "{\"type\":\"sub\",\"topic\":[\"log\"]}");

    bb_sink_ws_set_malloc(test_failing_malloc);
    test_alloc_fail_at = 0;
    test_alloc_reset();

    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());

    bb_sink_ws_set_malloc(NULL);
    test_alloc_fail_at = -1;
}

// (c) unsubscribed client (never sent a sub frame) receives nothing.
void test_bb_sink_ws_unsubscribed_client_receives_nothing(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(7, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("info", sample_power, NULL);

    // No sub frame injected for fd=7.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());

    // Log event inject also delivers nothing to unsubscribed client.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

// ---------------------------------------------------------------------------
// Alloc-failure paths (host-reachable)
// ---------------------------------------------------------------------------

// publish malloc fail → returns BB_ERR_NO_SPACE, nothing broadcast.
void test_bb_sink_ws_publish_malloc_fail_returns_no_space(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(0, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 0, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Inject a failing malloc so sink_ws_publish returns BB_ERR_NO_SPACE.
    bb_sink_ws_set_malloc(test_failing_malloc);
    test_alloc_fail_at = 0;
    test_alloc_reset();

    bb_err_t err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());

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
    bb_websocket_host_set_client_active(3, true);
    bb_websocket_host_set_client_active(5, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");
    inject_sub(req, 5, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Pre-suspend: both clients receive.
    bb_err_t err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(2, bb_websocket_host_async_count());
    bb_websocket_host_async_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());

    // Post-suspend: no client receives.
    err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":2}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

// disconnect notification clears subscription state for that fd — an fd
// reused by a new connection (LWIP fd reuse) never inherits the prior
// client's subscription filter (HIGH #2 fix: previously only suspend cleared
// subs, not a plain socket disconnect).
void test_sink_ws_disconnect_clears_client_sub_state(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Pre-disconnect: fd=3 receives.
    bb_err_t err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
    bb_websocket_host_async_reset();

    // Simulate the socket disconnecting (fd=3 goes away) without a suspend.
    bb_websocket_host_simulate_disconnect(3);
    bb_websocket_host_set_client_active(3, false);

    // A new connection reuses fd=3 (LWIP fd reuse) but never re-subscribes —
    // it must NOT inherit the old subscription.
    bb_websocket_host_set_client_active(3, true);
    err = s.publish(s.ctx, "m/h/power", "{\"ts_ms\":1,\"data\":{\"v\":2}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
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
    bb_websocket_host_set_client_active(2, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_suspend());
    bb_sink_ws_resume();

    // Re-inject subscription (suspend cleared sub table).
    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 2, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    bb_err_t err = s.publish(s.ctx, "m/h/info", "{\"ts_ms\":1,\"data\":{\"x\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
}

// (d) malformed sub frame is ignored safely — state for the fd unchanged.
void test_bb_sink_ws_malformed_sub_frame_ignored(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(8, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);

    // Inject various malformed payloads — none should crash.
    const char *bad[] = {
        "not json",
        "{}",
        "{\"sub\":}",
        "{\"other\":[\"foo\"]}",
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bb_websocket_host_set_inject_fd(8);
        bb_websocket_frame_t frame = {
            .final   = true,
            .type    = BB_WS_TYPE_TEXT,
            .payload = (uint8_t *)bad[i],
            .len     = strlen(bad[i]),
        };
        bb_err_t err = bb_websocket_host_inject_frame(req, &frame);
        TEST_ASSERT_EQUAL(BB_OK, err);
        bb_websocket_host_set_inject_fd(-1);
    }
    bb_websocket_host_async_reset();

    // fd=8 still has no valid subscriptions → publish delivers nothing.
    bb_pub_sink_t s2;
    bb_sink_ws_init(NULL, &s2);
    bb_err_t err = s2.publish(s2.ctx, "m/h/info", "{\"ts_ms\":1,\"data\":{\"x\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

// ---------------------------------------------------------------------------
// Subscription: exact subtopic match (not just coarse group)
// ---------------------------------------------------------------------------

void test_bb_sink_ws_sub_exact_subtopic_match(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(0, true);
    bb_websocket_host_set_client_active(1, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);

    // fd=0 subscribed to exact "mining" subtopic only.
    inject_sub(req, 0, "{\"type\":\"sub\",\"topic\":[\"mining\"]}");
    // fd=1 subscribed to "pool" only.
    inject_sub(req, 1, "{\"type\":\"sub\",\"topic\":[\"pool\"]}");

    // Publish "mining": only fd=0 should receive.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
    TEST_ASSERT_EQUAL(0, bb_websocket_host_async_at(0)->fd);
    bb_websocket_host_async_reset();

    // Publish "pool": only fd=1 should receive.
    err = s.publish(s.ctx, "m/h/pool", "{\"ts_ms\":1,\"data\":{\"p\":1}}", 26, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
    TEST_ASSERT_EQUAL(1, bb_websocket_host_async_at(0)->fd);
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
    bb_websocket_host_set_client_active(4, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
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
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
    bb_websocket_host_async_reset();

    // "log" exact opt-in should also arrive.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"m\"}");
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
}

// ---------------------------------------------------------------------------
// Subscription: coarse "events" group
// ---------------------------------------------------------------------------

void test_bb_sink_ws_sub_events_group(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(2, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 2, "{\"type\":\"sub\",\"topic\":[\"events\"]}");

    // "events" subtopic should arrive.
    bb_err_t err = s.publish(s.ctx, "m/h/events", "{\"ts_ms\":1,\"data\":{\"type\":\"share\"}}", 35, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
    bb_websocket_host_async_reset();

    // "mining" subtopic should NOT arrive.
    err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
}

// ---------------------------------------------------------------------------
// Subscription: replace (re-sub overwrites previous)
// ---------------------------------------------------------------------------

void test_bb_sink_ws_sub_replace_on_resub(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);

    // First sub: telemetry.
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");

    // Confirm telemetry delivers.
    bb_err_t err = s.publish(s.ctx, "m/h/info", "{\"ts_ms\":1,\"data\":{}}", 21, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
    bb_websocket_host_async_reset();

    // Re-sub to log only — telemetry should no longer deliver.
    inject_sub(req, 3, "{\"type\":\"sub\",\"topic\":[\"log\"]}");

    err = s.publish(s.ctx, "m/h/info", "{\"ts_ms\":1,\"data\":{}}", 21, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());

    // But log events should now deliver.
    bb_sink_ws_host_inject_log_event("{\"ts\":1,\"level\":\"I\",\"tag\":\"t\",\"msg\":\"after resub\"}");
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
}

// ---------------------------------------------------------------------------
// Inbound envelope demux: legacy back-compat, reserved types, pool sizing
// ---------------------------------------------------------------------------

// Legacy {"sub":[...]} (no "type" envelope) is still accepted.
void test_bb_sink_ws_legacy_sub_frame_back_compat(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(2, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    // Legacy frame, no "type" key.
    inject_sub(req, 2, "{\"sub\":[\"telemetry\"]}");

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_websocket_host_async_count());
}

// An envelope with a "type" other than "sub" (e.g. "cmd") is RESERVED —
// ignored-with-log, no subscription change, no crash.
void test_bb_sink_ws_reserved_type_ignored(void)
{
    ws_sink_setup();
    bb_websocket_host_set_client_active(3, true);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    inject_sub(req, 3, "{\"type\":\"cmd\",\"topic\":[\"telemetry\"]}");

    // No subscription was created for fd=3 — publish delivers nothing.
    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_websocket_host_async_count());
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
        bb_websocket_host_set_client_active(fds[i], true);
    }

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_ws_init(NULL, &s));

    bb_http_request_t *req = NULL;
    bb_websocket_host_capture_begin(&req);
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        inject_sub(req, fds[i], "{\"type\":\"sub\",\"topic\":[\"telemetry\"]}");
    }

    bb_err_t err = s.publish(s.ctx, "m/h/mining", "{\"ts_ms\":1,\"data\":{\"hr\":1}}", 27, false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    // Only the first 5 (pool capacity) actually got a subscription slot.
    TEST_ASSERT_EQUAL_INT(5, bb_websocket_host_async_count());
}
