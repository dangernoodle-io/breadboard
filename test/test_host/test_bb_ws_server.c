// Host tests for bb_ws_server.
// Exercises all host-compiled branches: registration, recv, send,
// broadcast, null-safety, force-fail hooks, and async capture.
// Coverage target: 100% line/function/branch.
//
// No setUp/tearDown defined here — test_main.c owns those.
// Each test calls ws_test_setup() locally for a clean slate.

#include "unity.h"
#include "bb_ws_server.h"
#include "bb_ws_server_host.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_prov_gate.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Test handler stubs
// ---------------------------------------------------------------------------

static bb_ws_server_frame_type_t s_last_handler_frame_type;
static size_t s_last_handler_payload_len;
static char   s_last_handler_payload[256];

static bb_err_t echo_handler(bb_http_request_t *req,
                              const bb_ws_server_frame_t *frame)
{
    s_last_handler_frame_type   = frame->type;
    s_last_handler_payload_len  = frame->len;
    if (frame->payload && frame->len < sizeof(s_last_handler_payload)) {
        memcpy(s_last_handler_payload, frame->payload, frame->len);
        s_last_handler_payload[frame->len] = '\0';
    }
    bb_ws_server_frame_t reply = {
        .final   = true,
        .type    = frame->type,
        .payload = frame->payload,
        .len     = frame->len,
    };
    return bb_ws_server_send_frame(req, &reply);
}

static bb_err_t fail_handler(bb_http_request_t *req,
                              const bb_ws_server_frame_t *frame)
{
    (void)req;
    (void)frame;
    return BB_ERR_INVALID_STATE;
}

// ---------------------------------------------------------------------------
// Local reset helper — called at the start of each test
// ---------------------------------------------------------------------------

static void ws_test_setup(void)
{
    bb_ws_server_host_reset_captures();
    bb_http_route_registry_clear();
    s_last_handler_frame_type  = BB_WS_TYPE_TEXT;
    s_last_handler_payload_len = 0;
    memset(s_last_handler_payload, 0, sizeof(s_last_handler_payload));
}

// ---------------------------------------------------------------------------
// Registration tests
// ---------------------------------------------------------------------------

void test_bb_ws_server_register_endpoint_null_server_ok(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_ws_server_register_endpoint_null_path(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_register_endpoint(NULL, NULL, echo_handler);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_register_endpoint_null_handler(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_register_endpoint(NULL, "/ws", NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_register_endpoint_force_fail(void)
{
    ws_test_setup();
    bb_ws_server_host_force_register_fail(true);
    bb_err_t err = bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    bb_ws_server_host_force_register_fail(false);
}

// ---------------------------------------------------------------------------
// Recv frame tests
// ---------------------------------------------------------------------------

void test_bb_ws_server_recv_frame_null_req(void)
{
    ws_test_setup();
    bb_ws_server_frame_t frame = {0};
    bb_err_t err = bb_ws_server_recv_frame(NULL, &frame, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_recv_frame_null_frame(void)
{
    ws_test_setup();
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    bb_err_t err = bb_ws_server_recv_frame(req, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_recv_frame_probe(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    // Inject a frame (this calls the handler internally; that's fine —
    // we're testing that the injected state is then probe-readable via
    // recv_frame called independently).
    uint8_t data[] = "hello";
    bb_ws_server_frame_t in = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = data,
        .len     = 5,
    };
    bb_ws_server_host_inject_frame(req, &in);

    // Probe call: max_len=0 returns frame len without a payload copy.
    bb_ws_server_frame_t probe = {0};
    bb_err_t err = bb_ws_server_recv_frame(req, &probe, 0);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(5, probe.len);
    TEST_ASSERT_NULL(probe.payload);
}

void test_bb_ws_server_recv_frame_force_fail(void)
{
    ws_test_setup();
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    bb_ws_server_host_force_recv_fail(true);
    bb_ws_server_frame_t frame = {0};
    bb_err_t err = bb_ws_server_recv_frame(req, &frame, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    bb_ws_server_host_force_recv_fail(false);
}

void test_bb_ws_server_recv_frame_with_payload(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    uint8_t data[] = "world";
    bb_ws_server_frame_t in = {
        .final   = true,
        .type    = BB_WS_TYPE_BINARY,
        .payload = data,
        .len     = 5,
    };
    bb_ws_server_host_inject_frame(req, &in);

    uint8_t buf[16] = {0};
    bb_ws_server_frame_t recv_f = {
        .payload = buf,
        .len     = 5,
    };
    bb_err_t err = bb_ws_server_recv_frame(req, &recv_f, 5);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_MEMORY("world", buf, 5);
}

void test_bb_ws_server_recv_frame_null_payload_buffer_zero_len(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    bb_ws_server_frame_t in = {
        .final   = true,
        .type    = BB_WS_TYPE_PING,
        .payload = NULL,
        .len     = 0,
    };
    bb_ws_server_host_inject_frame(req, &in);

    bb_ws_server_frame_t recv_f = { .payload = NULL, .len = 0 };
    bb_err_t err = bb_ws_server_recv_frame(req, &recv_f, 4);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

// ---------------------------------------------------------------------------
// Send frame tests
// ---------------------------------------------------------------------------

void test_bb_ws_server_send_frame_null_req(void)
{
    ws_test_setup();
    bb_ws_server_frame_t frame = { .final = true, .type = BB_WS_TYPE_TEXT,
                                   .payload = (uint8_t*)"hi", .len = 2 };
    bb_err_t err = bb_ws_server_send_frame(NULL, &frame);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_send_frame_null_frame(void)
{
    ws_test_setup();
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    bb_err_t err = bb_ws_server_send_frame(req, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_send_frame_captures_payload(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    uint8_t payload[] = "ping";
    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = payload,
        .len     = 4,
    };
    bb_err_t err = bb_ws_server_send_frame(req, &frame);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_ws_server_host_capture_t cap = {0};
    bb_ws_server_host_capture_sent_frame(&cap);
    TEST_ASSERT_EQUAL(BB_WS_TYPE_TEXT, cap.type);
    TEST_ASSERT_TRUE(cap.final);
    TEST_ASSERT_EQUAL(4, cap.len);
    TEST_ASSERT_NOT_NULL(cap.payload);
    TEST_ASSERT_EQUAL_MEMORY("ping", cap.payload, 4);
    TEST_ASSERT_EQUAL(BB_OK, cap.err);
    bb_ws_server_host_capture_free(&cap);
}

void test_bb_ws_server_send_frame_force_fail(void)
{
    ws_test_setup();
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    bb_ws_server_host_force_send_fail(true);
    uint8_t payload[] = "x";
    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = payload,
        .len     = 1,
    };
    bb_err_t err = bb_ws_server_send_frame(req, &frame);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_ws_server_host_force_send_fail(false);
}

void test_bb_ws_server_send_frame_empty_payload(void)
{
    ws_test_setup();
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_PING,
        .payload = NULL,
        .len     = 0,
    };
    bb_err_t err = bb_ws_server_send_frame(req, &frame);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_ws_server_host_capture_t cap = {0};
    bb_ws_server_host_capture_sent_frame(&cap);
    TEST_ASSERT_EQUAL(0, cap.len);
    TEST_ASSERT_NULL(cap.payload);
    bb_ws_server_host_capture_free(&cap);
}

void test_bb_ws_server_capture_sent_frame_second_call_empty(void)
{
    ws_test_setup();
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t p[] = "a";
    bb_ws_server_frame_t f = { .final=true, .type=BB_WS_TYPE_TEXT, .payload=p, .len=1 };
    bb_ws_server_send_frame(req, &f);

    bb_ws_server_host_capture_t cap1 = {0};
    bb_ws_server_host_capture_sent_frame(&cap1);
    bb_ws_server_host_capture_free(&cap1);

    // Second drain: ownership was transferred, should be empty.
    bb_ws_server_host_capture_t cap2 = {0};
    bb_ws_server_host_capture_sent_frame(&cap2);
    TEST_ASSERT_NULL(cap2.payload);
    TEST_ASSERT_EQUAL(0, cap2.len);
}

void test_bb_ws_server_capture_sent_frame_null(void)
{
    // Must not crash.
    bb_ws_server_host_capture_sent_frame(NULL);
}

void test_bb_ws_server_capture_free_null(void)
{
    // Must not crash.
    bb_ws_server_host_capture_free(NULL);
}

// ---------------------------------------------------------------------------
// Echo round-trip (inject → handler → send captured)
// ---------------------------------------------------------------------------

void test_bb_ws_server_echo_roundtrip_text(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    uint8_t data[] = "hello websocket";
    bb_ws_server_frame_t in = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = data,
        .len     = sizeof(data) - 1,
    };
    bb_err_t rc = bb_ws_server_host_inject_frame(req, &in);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_ws_server_host_capture_t cap = {0};
    bb_ws_server_host_capture_sent_frame(&cap);
    TEST_ASSERT_EQUAL(BB_WS_TYPE_TEXT, cap.type);
    TEST_ASSERT_TRUE(cap.final);
    TEST_ASSERT_EQUAL(sizeof(data) - 1, cap.len);
    TEST_ASSERT_NOT_NULL(cap.payload);
    TEST_ASSERT_EQUAL_MEMORY("hello websocket", cap.payload, cap.len);
    bb_ws_server_host_capture_free(&cap);
}

void test_bb_ws_server_echo_roundtrip_binary(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);

    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    bb_ws_server_frame_t in = {
        .final   = true,
        .type    = BB_WS_TYPE_BINARY,
        .payload = data,
        .len     = sizeof(data),
    };
    bb_ws_server_host_inject_frame(req, &in);

    bb_ws_server_host_capture_t cap = {0};
    bb_ws_server_host_capture_sent_frame(&cap);
    TEST_ASSERT_EQUAL(BB_WS_TYPE_BINARY, cap.type);
    TEST_ASSERT_EQUAL(sizeof(data), cap.len);
    TEST_ASSERT_EQUAL_MEMORY(data, cap.payload, sizeof(data));
    bb_ws_server_host_capture_free(&cap);
}

void test_bb_ws_server_inject_frame_no_handler(void)
{
    ws_test_setup();
    // No handler registered.
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t data[] = "x";
    bb_ws_server_frame_t in = {
        .final=true, .type=BB_WS_TYPE_TEXT, .payload=data, .len=1
    };
    bb_err_t rc = bb_ws_server_host_inject_frame(req, &in);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

void test_bb_ws_server_inject_frame_null_req(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);
    uint8_t data[] = "x";
    bb_ws_server_frame_t in = {
        .final=true, .type=BB_WS_TYPE_TEXT, .payload=data, .len=1
    };
    bb_err_t rc = bb_ws_server_host_inject_frame(NULL, &in);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

void test_bb_ws_server_inject_frame_null_frame(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    bb_err_t rc = bb_ws_server_host_inject_frame(req, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

void test_bb_ws_server_handler_returns_error(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", fail_handler);
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t d[] = "x";
    bb_ws_server_frame_t in = { .final=true, .type=BB_WS_TYPE_TEXT, .payload=d, .len=1 };
    bb_err_t rc = bb_ws_server_host_inject_frame(req, &in);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

// ---------------------------------------------------------------------------
// Broadcast async tests
// ---------------------------------------------------------------------------

static int s_cb_count;
static bb_err_t s_cb_err;

static void async_cb(bb_err_t err, int fd, void *ctx)
{
    (void)fd;
    (void)ctx;
    s_cb_count++;
    s_cb_err = err;
}

void test_bb_ws_server_broadcast_frame_async_null_frame(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_broadcast_frame_async(NULL, 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_broadcast_frame_async_captures(void)
{
    ws_test_setup();
    s_cb_count = 0;
    s_cb_err   = -1;

    uint8_t p[] = "broadcast";
    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = p,
        .len     = sizeof(p) - 1,
    };
    bb_err_t err = bb_ws_server_broadcast_frame_async(NULL, 5, &frame, async_cb, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, s_cb_count);
    TEST_ASSERT_EQUAL(BB_OK, s_cb_err);
    TEST_ASSERT_EQUAL(1, bb_ws_server_host_async_count());

    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(5, a->fd);
    TEST_ASSERT_EQUAL(BB_WS_TYPE_TEXT, a->type);
    TEST_ASSERT_EQUAL(sizeof(p) - 1, a->len);
    TEST_ASSERT_NOT_NULL(a->payload);
    TEST_ASSERT_EQUAL_MEMORY("broadcast", a->payload, sizeof(p) - 1);
}

void test_bb_ws_server_broadcast_frame_async_null_cb(void)
{
    ws_test_setup();
    uint8_t p[] = "x";
    bb_ws_server_frame_t frame = { .final=true, .type=BB_WS_TYPE_TEXT,
                                   .payload=p, .len=1 };
    bb_err_t err = bb_ws_server_broadcast_frame_async(NULL, 0, &frame, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_ws_server_broadcast_frame_async_empty_payload(void)
{
    ws_test_setup();
    bb_ws_server_frame_t frame = { .final=true, .type=BB_WS_TYPE_PING,
                                   .payload=NULL, .len=0 };
    bb_err_t err = bb_ws_server_broadcast_frame_async(NULL, 1, &frame, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(0, a->len);
    TEST_ASSERT_NULL(a->payload);
}

void test_bb_ws_server_broadcast_frame_async_force_alloc_fail(void)
{
    ws_test_setup();
    s_cb_count = 0;
    s_cb_err   = BB_OK;
    bb_ws_server_host_force_async_alloc_fail(true);
    uint8_t p[] = "x";
    bb_ws_server_frame_t frame = { .final=true, .type=BB_WS_TYPE_TEXT,
                                   .payload=p, .len=1 };
    bb_err_t err = bb_ws_server_broadcast_frame_async(NULL, 3, &frame, async_cb, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL(1, s_cb_count);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, s_cb_err);
    bb_ws_server_host_force_async_alloc_fail(false);
}

void test_bb_ws_server_async_at_out_of_range(void)
{
    ws_test_setup();
    const bb_ws_server_host_async_capture_t *a = bb_ws_server_host_async_at(-1);
    TEST_ASSERT_NULL(a);
    a = bb_ws_server_host_async_at(100);
    TEST_ASSERT_NULL(a);
}

void test_bb_ws_server_async_reset(void)
{
    ws_test_setup();
    uint8_t p[] = "x";
    bb_ws_server_frame_t frame = { .final=true, .type=BB_WS_TYPE_TEXT,
                                   .payload=p, .len=1 };
    bb_ws_server_broadcast_frame_async(NULL, 0, &frame, NULL, NULL);
    TEST_ASSERT_EQUAL(1, bb_ws_server_host_async_count());
    bb_ws_server_host_async_reset();
    TEST_ASSERT_EQUAL(0, bb_ws_server_host_async_count());
}

// ---------------------------------------------------------------------------
// is_client / client_active tests
// ---------------------------------------------------------------------------

void test_bb_ws_server_is_client_false_by_default(void)
{
    ws_test_setup();
    TEST_ASSERT_FALSE(bb_ws_server_is_client(NULL, 0));
}

void test_bb_ws_server_is_client_after_set(void)
{
    ws_test_setup();
    bb_ws_server_host_set_client_active(2, true);
    TEST_ASSERT_TRUE(bb_ws_server_is_client(NULL, 2));
    bb_ws_server_host_set_client_active(2, false);
    TEST_ASSERT_FALSE(bb_ws_server_is_client(NULL, 2));
}

void test_bb_ws_server_is_client_negative_fd(void)
{
    ws_test_setup();
    TEST_ASSERT_FALSE(bb_ws_server_is_client(NULL, -1));
}

void test_bb_ws_server_is_client_fd_too_large(void)
{
    ws_test_setup();
    TEST_ASSERT_FALSE(bb_ws_server_is_client(NULL, BB_WS_SERVER_MAX_FD + 10));
}

void test_bb_ws_server_set_client_active_clear_all(void)
{
    ws_test_setup();
    bb_ws_server_host_set_client_active(0, true);
    bb_ws_server_host_set_client_active(1, true);
    bb_ws_server_host_set_client_active(-1, false);  // fd=-1 clears all
    TEST_ASSERT_FALSE(bb_ws_server_is_client(NULL, 0));
    TEST_ASSERT_FALSE(bb_ws_server_is_client(NULL, 1));
}

// ---------------------------------------------------------------------------
// broadcast_all tests
// ---------------------------------------------------------------------------

void test_bb_ws_server_broadcast_all_no_clients(void)
{
    ws_test_setup();
    uint8_t p[] = "broadcast";
    bb_ws_server_frame_t frame = { .final=true, .type=BB_WS_TYPE_TEXT,
                                   .payload=p, .len=sizeof(p)-1 };
    bb_err_t err = bb_ws_server_broadcast_all(NULL, &frame, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(0, bb_ws_server_host_async_count());
}

void test_bb_ws_server_broadcast_all_two_clients(void)
{
    ws_test_setup();
    s_cb_count = 0;
    bb_ws_server_host_set_client_active(3, true);
    bb_ws_server_host_set_client_active(7, true);

    uint8_t p[] = "hello";
    bb_ws_server_frame_t frame = { .final=true, .type=BB_WS_TYPE_TEXT,
                                   .payload=p, .len=sizeof(p)-1 };
    bb_err_t err = bb_ws_server_broadcast_all(NULL, &frame, async_cb, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, bb_ws_server_host_async_count());
    TEST_ASSERT_EQUAL(2, s_cb_count);
}

void test_bb_ws_server_broadcast_all_null_frame(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_broadcast_all(NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// ---------------------------------------------------------------------------
// Described endpoint + OpenAPI registry
// ---------------------------------------------------------------------------

static bb_err_t noop_handler(bb_http_request_t *req,
                              const bb_ws_server_frame_t *frame)
{
    (void)req;
    (void)frame;
    return BB_OK;
}

static const bb_route_response_t s_ws_responses[] = {
    {
        .status      = 101,
        .description = "WebSocket upgrade (Switching Protocols)",
    },
    { .status = 0 },
};

static const bb_route_t s_ws_route = {
    .method       = BB_HTTP_GET,
    .path         = "/ws",
    .tag          = "websocket",
    .summary      = "WebSocket echo endpoint (upgrade via GET; x-protocol: websocket)",
    .operation_id = "wsConnect",
    .responses    = s_ws_responses,
    .handler      = NULL,
};

void test_bb_ws_server_register_described_endpoint_ok(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_register_described_endpoint(
        NULL, "/ws", noop_handler, &s_ws_route);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());
}

void test_bb_ws_server_register_described_endpoint_null_path(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_register_described_endpoint(
        NULL, NULL, noop_handler, &s_ws_route);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_register_described_endpoint_null_handler(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_register_described_endpoint(
        NULL, "/ws", NULL, &s_ws_route);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_register_described_endpoint_null_descriptor(void)
{
    ws_test_setup();
    bb_err_t err = bb_ws_server_register_described_endpoint(
        NULL, "/ws", noop_handler, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_ws_server_register_described_endpoint_propagates_register_fail(void)
{
    ws_test_setup();
    bb_ws_server_host_force_register_fail(true);
    bb_err_t err = bb_ws_server_register_described_endpoint(
        NULL, "/ws", noop_handler, &s_ws_route);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());
    bb_ws_server_host_force_register_fail(false);
}

// ---------------------------------------------------------------------------
// reset_captures clears everything
// ---------------------------------------------------------------------------

void test_bb_ws_server_reset_captures_clears_state(void)
{
    ws_test_setup();
    bb_ws_server_register_endpoint(NULL, "/ws", echo_handler);
    bb_ws_server_host_set_client_active(4, true);
    bb_ws_server_host_reset_captures();

    TEST_ASSERT_FALSE(bb_ws_server_is_client(NULL, 4));

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t d[] = "x";
    bb_ws_server_frame_t in = { .final=true, .type=BB_WS_TYPE_TEXT, .payload=d, .len=1 };
    bb_err_t rc = bb_ws_server_host_inject_frame(req, &in);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

// ---------------------------------------------------------------------------
// Open-connection counter (B1-443)
// ---------------------------------------------------------------------------

void test_bb_ws_server_open_count_zero_by_default(void)
{
    ws_test_setup();
    TEST_ASSERT_EQUAL(0, bb_ws_server_open_count());
}

void test_bb_ws_server_open_count_increments_on_simulated_open(void)
{
    ws_test_setup();
    bb_ws_server_host_simulate_open();
    TEST_ASSERT_EQUAL(1, bb_ws_server_open_count());
    bb_ws_server_host_simulate_open();
    TEST_ASSERT_EQUAL(2, bb_ws_server_open_count());
}

void test_bb_ws_server_open_count_decrements_on_simulated_close(void)
{
    ws_test_setup();
    bb_ws_server_host_simulate_open();
    bb_ws_server_host_simulate_open();
    bb_ws_server_host_simulate_close();
    TEST_ASSERT_EQUAL(1, bb_ws_server_open_count());
    bb_ws_server_host_simulate_close();
    TEST_ASSERT_EQUAL(0, bb_ws_server_open_count());
}

void test_bb_ws_server_open_count_close_clamps_at_zero(void)
{
    ws_test_setup();
    bb_ws_server_host_simulate_close();
    TEST_ASSERT_EQUAL(0, bb_ws_server_open_count());
}

void test_bb_ws_server_open_count_reset_by_reset_captures(void)
{
    ws_test_setup();
    bb_ws_server_host_simulate_open();
    bb_ws_server_host_reset_captures();
    TEST_ASSERT_EQUAL(0, bb_ws_server_open_count());
}

// ---------------------------------------------------------------------------
// Disconnect notification
// ---------------------------------------------------------------------------

static int  s_disc_fd_seen  = -1;
static void *s_disc_ctx_seen = NULL;
static int  s_disc_call_count = 0;

static void disc_cb(int fd, void *ctx)
{
    s_disc_fd_seen    = fd;
    s_disc_ctx_seen   = ctx;
    s_disc_call_count++;
}

void test_bb_ws_server_disconnect_cb_invoked_with_fd_and_ctx(void)
{
    ws_test_setup();
    s_disc_fd_seen = -1;
    s_disc_ctx_seen = NULL;
    s_disc_call_count = 0;

    int marker;
    bb_ws_server_set_disconnect_cb(disc_cb, &marker);
    bb_ws_server_host_simulate_disconnect(7);

    TEST_ASSERT_EQUAL(1, s_disc_call_count);
    TEST_ASSERT_EQUAL(7, s_disc_fd_seen);
    TEST_ASSERT_EQUAL_PTR(&marker, s_disc_ctx_seen);
}

void test_bb_ws_server_disconnect_cb_null_is_noop(void)
{
    ws_test_setup();
    // No callback registered — must not crash.
    bb_ws_server_host_simulate_disconnect(3);
}

void test_bb_ws_server_disconnect_cb_cleared_by_reset_captures(void)
{
    ws_test_setup();
    s_disc_call_count = 0;
    bb_ws_server_set_disconnect_cb(disc_cb, NULL);
    bb_ws_server_host_reset_captures();
    bb_ws_server_host_simulate_disconnect(1);
    TEST_ASSERT_EQUAL(0, s_disc_call_count);
}

// ---------------------------------------------------------------------------
// req_fd / inject_fd (B1-...: bb_pub/bb_sink_* cluster deletion left these
// still-public primitives with no direct test — restore real coverage)
// ---------------------------------------------------------------------------

void test_bb_ws_server_req_fd_default_is_negative_one(void)
{
    ws_test_setup();
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    TEST_ASSERT_EQUAL(-1, bb_ws_server_req_fd(req));
}

void test_bb_ws_server_req_fd_reflects_injected_fd(void)
{
    ws_test_setup();
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    bb_ws_server_host_set_inject_fd(42);
    TEST_ASSERT_EQUAL(42, bb_ws_server_req_fd(req));
    // Clear back to default so later tests are unaffected.
    bb_ws_server_host_set_inject_fd(-1);
    TEST_ASSERT_EQUAL(-1, bb_ws_server_req_fd(req));
}

// ---------------------------------------------------------------------------
// close_client (host — no-op, always BB_OK)
// ---------------------------------------------------------------------------

void test_bb_ws_server_close_client_returns_ok(void)
{
    ws_test_setup();
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_close_client(NULL, 5));
}

// ---------------------------------------------------------------------------
// connect callback registration + simulate_connect
// ---------------------------------------------------------------------------

static bb_http_handle_t s_conn_server_seen = (bb_http_handle_t)1;
static int               s_conn_fd_seen     = -1;
static void             *s_conn_ctx_seen    = NULL;
static int               s_conn_call_count  = 0;

static void conn_cb(bb_http_handle_t server, int fd, void *ctx)
{
    s_conn_server_seen = server;
    s_conn_fd_seen      = fd;
    s_conn_ctx_seen     = ctx;
    s_conn_call_count++;
}

void test_bb_ws_server_connect_cb_invoked_with_server_fd_ctx(void)
{
    ws_test_setup();
    s_conn_server_seen = (bb_http_handle_t)1;  // sentinel, must be overwritten with NULL
    s_conn_fd_seen      = -1;
    s_conn_ctx_seen     = NULL;
    s_conn_call_count   = 0;

    int marker;
    bb_ws_server_set_connect_cb(conn_cb, &marker);
    bb_ws_server_host_simulate_connect(NULL, 9);

    TEST_ASSERT_EQUAL(1, s_conn_call_count);
    TEST_ASSERT_NULL(s_conn_server_seen);
    TEST_ASSERT_EQUAL(9, s_conn_fd_seen);
    TEST_ASSERT_EQUAL_PTR(&marker, s_conn_ctx_seen);
}

void test_bb_ws_server_connect_cb_null_is_noop(void)
{
    ws_test_setup();
    // No callback registered — must not crash.
    bb_ws_server_host_simulate_connect(NULL, 3);
}

void test_bb_ws_server_connect_cb_cleared_by_reset_captures(void)
{
    ws_test_setup();
    s_conn_call_count = 0;
    bb_ws_server_set_connect_cb(conn_cb, NULL);
    bb_ws_server_host_reset_captures();
    bb_ws_server_host_simulate_connect(NULL, 1);
    TEST_ASSERT_EQUAL(0, s_conn_call_count);
}

// ---------------------------------------------------------------------------
// Provisioning gate (B1-1348) — exercises the SAME two insertion points as
// the device handlers in platform/espidf/bb_ws_server/bb_ws_server.c
// (ws_pre_handshake_gate_cb and ws_shim_handler's per-frame check) through
// their host mirrors (bb_ws_server_host_handshake, bb_ws_server_host_
// inject_frame) — NOT just the underlying pure bb_http_prov_gate_allow()
// predicate (already covered by test_bb_http_prov_gate.c). A suite that
// only exercised bb_http_prov_gate_allow() directly would miss either call
// site forgetting to invoke it.
//
// setUp() (test_main.c) already resets the allowlist + injected active-fn
// before every test (bb_http_prov_gate_reset()), so every test below starts
// from a clean, inactive gate.
// ---------------------------------------------------------------------------

static bool s_ws_mock_prov_active;

static bool ws_mock_prov_active_fn(void)
{
    return s_ws_mock_prov_active;
}

static bool s_gate_handler_invoked;

static bb_err_t gate_test_handler(bb_http_request_t *req,
                                  const bb_ws_server_frame_t *frame)
{
    (void)req;
    (void)frame;
    s_gate_handler_invoked = true;
    return BB_OK;
}

static void arm_ws_mock_active_fn(void)
{
    s_ws_mock_prov_active = false;
    bb_http_prov_gate_set_active_fn(ws_mock_prov_active_fn);
}

static void ws_gate_teardown(void)
{
    bb_http_prov_gate_set_active_fn(NULL);
    s_gate_handler_invoked = false;
}

// ---- Insertion point 1/2 — ws_pre_handshake_gate_cb (host mirror:
// bb_ws_server_host_handshake) ----

void test_bb_ws_server_prov_gate_handshake_active_non_allowlisted_denies(void)
{
    ws_test_setup();
    arm_ws_mock_active_fn();
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);
    // deliberately NOT allowlisted
    s_ws_mock_prov_active = true;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ws_server_host_handshake());
    // Pins the fix: a denied handshake must send an explicit 404, not just
    // silently close (see ws_pre_handshake_gate_cb's comment) — otherwise a
    // bare rejected handshake is distinguishable on the wire from a
    // genuinely-unregistered WS path.
    TEST_ASSERT_TRUE(bb_ws_server_host_handshake_404_sent());

    ws_gate_teardown();
}

void test_bb_ws_server_prov_gate_handshake_active_allowlisted_allows(void)
{
    ws_test_setup();
    arm_ws_mock_active_fn();
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ws"));
    s_ws_mock_prov_active = true;

    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_host_handshake());
    TEST_ASSERT_FALSE(bb_ws_server_host_handshake_404_sent());

    ws_gate_teardown();
}

void test_bb_ws_server_prov_gate_handshake_inactive_allows(void)
{
    ws_test_setup();
    arm_ws_mock_active_fn();
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);
    // gate never armed active; not allowlisted either — must still allow.
    TEST_ASSERT_FALSE(s_ws_mock_prov_active);

    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_host_handshake());
    TEST_ASSERT_FALSE(bb_ws_server_host_handshake_404_sent());

    ws_gate_teardown();
}

// ---- Insertion point 2/2 — ws_shim_handler's per-frame check (host
// mirror: bb_ws_server_host_inject_frame) ----

void test_bb_ws_server_prov_gate_frame_active_non_allowlisted_denies(void)
{
    ws_test_setup();
    arm_ws_mock_active_fn();
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);
    // deliberately NOT allowlisted
    s_ws_mock_prov_active = true;

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t data[] = "hi";
    bb_ws_server_frame_t in = { .final = true, .type = BB_WS_TYPE_TEXT,
                                .payload = data, .len = sizeof(data) };
    bb_err_t err = bb_ws_server_host_inject_frame(req, &in);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_FALSE(s_gate_handler_invoked);

    ws_gate_teardown();
}

void test_bb_ws_server_prov_gate_frame_active_allowlisted_invokes_handler(void)
{
    ws_test_setup();
    arm_ws_mock_active_fn();
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ws"));
    s_ws_mock_prov_active = true;

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t data[] = "hi";
    bb_ws_server_frame_t in = { .final = true, .type = BB_WS_TYPE_TEXT,
                                .payload = data, .len = sizeof(data) };
    bb_err_t err = bb_ws_server_host_inject_frame(req, &in);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_gate_handler_invoked);

    ws_gate_teardown();
}

void test_bb_ws_server_prov_gate_frame_inactive_invokes_handler(void)
{
    ws_test_setup();
    arm_ws_mock_active_fn();
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);
    TEST_ASSERT_FALSE(s_ws_mock_prov_active);

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t data[] = "hi";
    bb_ws_server_frame_t in = { .final = true, .type = BB_WS_TYPE_TEXT,
                                .payload = data, .len = sizeof(data) };
    bb_err_t err = bb_ws_server_host_inject_frame(req, &in);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_gate_handler_invoked);

    ws_gate_teardown();
}

// ---- Established-connection scenario: a session that handshook while the
// gate was inactive must still be denied per-frame once provisioning
// becomes active later. This is the case B1-1348 calls out explicitly:
// gating the handshake alone is not sufficient because a WS session
// outlives the handshake call. ----

void test_bb_ws_server_prov_gate_session_open_before_active_denies_frame_once_active(void)
{
    ws_test_setup();
    arm_ws_mock_active_fn();
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);

    // Handshake while gate is inactive — allowed, session "opens".
    TEST_ASSERT_FALSE(s_ws_mock_prov_active);
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_host_handshake());

    // Provisioning becomes active afterward, with /ws never allowlisted —
    // the already-open session's next frame must now be denied.
    s_ws_mock_prov_active = true;

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t data[] = "hi";
    bb_ws_server_frame_t in = { .final = true, .type = BB_WS_TYPE_TEXT,
                                .payload = data, .len = sizeof(data) };
    bb_err_t err = bb_ws_server_host_inject_frame(req, &in);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_FALSE(s_gate_handler_invoked);

    ws_gate_teardown();
}

void test_bb_ws_server_prov_gate_no_active_fn_registered_unaffected(void)
{
    ws_test_setup();
    // Deliberately does NOT call arm_ws_mock_active_fn() — active-fn stays
    // NULL (setUp()'s bb_http_prov_gate_reset() already cleared it).
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);

    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_host_handshake());
    TEST_ASSERT_FALSE(bb_ws_server_host_handshake_404_sent());

    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t data[] = "hi";
    bb_ws_server_frame_t in = { .final = true, .type = BB_WS_TYPE_TEXT,
                                .payload = data, .len = sizeof(data) };
    bb_err_t err = bb_ws_server_host_inject_frame(req, &in);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_gate_handler_invoked);

    ws_gate_teardown();
}

// ---- Refusal-shape contrast: a denied handshake must be indistinguishable
// from a genuine miss (explicit 404), while a denied frame on an
// already-upgraded connection must NOT attempt an HTTP-shaped response at
// all -- see ws_pre_handshake_gate_cb's and ws_shim_handler's own comments
// for why the two insertion points intentionally differ. This test pins
// both halves of that contrast in one place so a future change that makes
// them accidentally match (in either direction) is caught. ----

void test_bb_ws_server_prov_gate_refusal_shape_handshake_404_frame_silent(void)
{
    ws_test_setup();
    arm_ws_mock_active_fn();
    bb_ws_server_register_endpoint(NULL, "/ws", gate_test_handler);
    // deliberately NOT allowlisted
    s_ws_mock_prov_active = true;

    // Handshake denial: must report the explicit-404 shape.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ws_server_host_handshake());
    TEST_ASSERT_TRUE(bb_ws_server_host_handshake_404_sent());

    // Frame denial on an (already-upgraded, per the real backend) session:
    // no response is captured -- the handler (the only thing that could
    // write a WS reply via bb_ws_server_send_frame) is never invoked, and
    // there is no HTTP-response capture to assert on at all, unlike the
    // handshake case above.
    bb_http_request_t *req = NULL;
    bb_ws_server_host_capture_begin(&req);
    uint8_t data[] = "hi";
    bb_ws_server_frame_t in = { .final = true, .type = BB_WS_TYPE_TEXT,
                                .payload = data, .len = sizeof(data) };
    bb_err_t err = bb_ws_server_host_inject_frame(req, &in);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_FALSE(s_gate_handler_invoked);

    bb_ws_server_host_capture_t cap = {0};
    bb_ws_server_host_capture_sent_frame(&cap);
    TEST_ASSERT_NULL(cap.payload);
    TEST_ASSERT_EQUAL(0, cap.len);
    bb_ws_server_host_capture_free(&cap);

    ws_gate_teardown();
}

// ---------------------------------------------------------------------------
// Call-site guard -- proves the real ESP-IDF backend
// (platform/espidf/bb_ws_server/bb_ws_server.c) gates on ctx->path, never
// req->uri. Source-scan rather than execution because that file is
// ESP-IDF-only and has no host build, and because the host mirror's own
// capture harness cannot reproduce the defect this guards against in the
// first place: bb_http_request_t is fully opaque on host (no uri field, no
// httpd_req_t-shaped struct at all -- see bb_ws_server_host.c's own comment
// on this), so bb_ws_server_host_inject_frame() was never able to check
// against anything OTHER than the correctly-populated s_registered_path.
// The host mirror was accidentally correct by construction even while the
// real ESP-IDF backend used req->uri (which is reliably "" on every real
// data frame -- see ws_pre_handshake_gate_cb's block comment in
// bb_ws_server.c for the full vendored-source trace) -- so
// test_bb_ws_server_prov_gate_frame_active_allowlisted_invokes_handler
// above passed throughout even while the device path silently denied every
// frame of an allowlisted session. This guard closes that blind spot the
// only way available without an on-device test: reading the real source.
//
// Path is relative to the worktree root, matching this repo's existing
// convention (see test_bb_log_secret.c's identical guard for bb_wifi_ap.c).
// ---------------------------------------------------------------------------

#define BB_WS_SERVER_SRC_PATH "platform/espidf/bb_ws_server/bb_ws_server.c"

// Sized generously above the current ~31KB source file -- see
// test_bb_log_secret.c's identical pattern for why (a truncated read that
// happens to cut off the target line would falsely FAIL this guard, not
// falsely pass it).
static char s_ws_source_buf[65536];

static size_t read_bb_ws_server_source(void)
{
    FILE *f = fopen(BB_WS_SERVER_SRC_PATH, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not open " BB_WS_SERVER_SRC_PATH
                                     " -- run host tests with cwd at the worktree root");
    size_t n = fread(s_ws_source_buf, 1, sizeof(s_ws_source_buf) - 1, f);
    s_ws_source_buf[n] = '\0';
    fclose(f);
    return n;
}

void test_bb_ws_server_prov_gate_never_keys_on_req_uri(void)
{
    read_bb_ws_server_source();
    // The exact pattern that shipped the frame-denial defect: gating on
    // req->uri, which is only ever populated for the handshake-completion
    // call and reliably "" for every real data frame.
    TEST_ASSERT_NULL_MESSAGE(strstr(s_ws_source_buf, "bb_http_prov_gate_check(BB_HTTP_GET, req->uri)"),
                              "bb_ws_server.c must never gate on req->uri -- it is unreliable "
                              "for WS data frames (see ws_pre_handshake_gate_cb's comment); "
                              "gate on ctx->path instead");
}

void test_bb_ws_server_prov_gate_both_insertion_points_key_on_ctx_path(void)
{
    read_bb_ws_server_source();
    // Both ws_pre_handshake_gate_cb and ws_shim_handler's per-frame check
    // must route through ctx->path -- count, not just presence, so a
    // regression that keeps one call site correct while the other reverts
    // (e.g. back to req->uri, or to some other field) is still caught.
    const char *needle = "bb_http_prov_gate_check(BB_HTTP_GET, ctx->path)";
    int count = 0;
    const char *p = s_ws_source_buf;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    TEST_ASSERT_EQUAL_MESSAGE(2, count,
        "bb_ws_server.c must gate BOTH ws_pre_handshake_gate_cb and "
        "ws_shim_handler's per-frame check on ctx->path");
}
