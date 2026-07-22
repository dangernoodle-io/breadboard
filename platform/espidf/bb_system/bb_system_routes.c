// bb_system_routes — POST /api/reboot handler (B1-1148 PR1).
//
// Portable: no ESP-IDF-specific includes at the top level. Compiled on both
// ESP-IDF and host (for unit tests) -- same posture as
// bb_storage_http_routes.c, which this file mirrors. The one on-device-only
// call, bb_system_restart_reason_at(), is gated behind #ifdef ESP_PLATFORM
// below: its host stub (platform/host/bb_system/bb_system_host.c) calls
// exit(0), which would kill the host test process if reached unconditionally.
//
// This PR is build-wiring + restart gating only -- no change to the parse
// logic (bb_system_reboot_parse_body(), already portable and already
// host-tested in test_bb_system.c) or to reboot_handler's own behavior.

#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_system.h"

#ifdef BB_SYSTEM_TESTING
#include "bb_system_test.h"
#include <string.h>
#endif

// Bound on the request body accepted for POST /api/reboot's optional
// {"ts": <epoch_s>, "detail": "<string>"} JSON. Both fields are optional;
// no body at all is a normal, tolerated request.
#define BB_SYSTEM_REBOOT_BODY_MAX 256

// ---------------------------------------------------------------------------
// Testing capture (host unit tests, B1-1148 finding 1) -- records what
// reboot_handler() actually RESOLVED (detail/ts, after the User-Agent
// fallback) immediately before the #ifdef ESP_PLATFORM restart call below.
// That call is the only consumer of the resolved values and is compiled out
// on host, so without this seam the fallback logic runs but is unobservable
// from host tests. Reusable by PR2 for asserting its parse results.
// ---------------------------------------------------------------------------

#ifdef BB_SYSTEM_TESTING
static struct {
    bool     called;
    char     detail[49];
    uint32_t ts;
} s_reboot_capture;

void bb_system_reboot_capture_reset_for_test(void)
{
    memset(&s_reboot_capture, 0, sizeof(s_reboot_capture));
}

bool bb_system_reboot_capture_get_for_test(char *out_detail, size_t out_detail_size, uint32_t *out_ts)
{
    if (!s_reboot_capture.called) return false;
    if (out_detail && out_detail_size > 0) {
        size_t len = strlen(s_reboot_capture.detail);
        if (len >= out_detail_size) len = out_detail_size - 1;
        memcpy(out_detail, s_reboot_capture.detail, len);
        out_detail[len] = '\0';
    }
    if (out_ts) *out_ts = s_reboot_capture.ts;
    return true;
}
#endif /* BB_SYSTEM_TESTING */

static bb_err_t reboot_handler(bb_http_request_t *req)
{
    // Read the (optional) body, bounded and NUL-terminated.
    char body[BB_SYSTEM_REBOOT_BODY_MAX + 1];
    int  body_len = 0;
    int  raw_len  = bb_http_req_body_len(req);
    if (raw_len > 0 && raw_len <= BB_SYSTEM_REBOOT_BODY_MAX) {
        int n = bb_http_req_recv(req, body, sizeof(body) - 1);
        if (n > 0) {
            body[n] = '\0';
            body_len = n;
        }
    }

    // Resolve the User-Agent fallback once, up front — the pure parse fn is
    // request-independent and takes the already-resolved string.
    char ua[49];
    const char *ua_p = (bb_http_req_get_header(req, "User-Agent", ua, sizeof(ua)) == BB_OK) ? ua : NULL;

    uint32_t ts = 0;
    char detail[49] = {0};
    bb_system_reboot_parse_body(body_len > 0 ? body : NULL, body_len, ua_p, &ts, detail, sizeof(detail));

    bb_http_json_obj_stream_t obj;
    bb_err_t rc = bb_http_resp_json_obj_begin(req, &obj);
    if (rc != BB_OK) return rc;
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting");
    rc = bb_http_resp_json_obj_end(&obj);
#ifdef BB_SYSTEM_TESTING
    s_reboot_capture.called = true;
    strncpy(s_reboot_capture.detail, detail, sizeof(s_reboot_capture.detail) - 1);
    s_reboot_capture.detail[sizeof(s_reboot_capture.detail) - 1] = '\0';
    s_reboot_capture.ts = ts;
#endif /* BB_SYSTEM_TESTING */
#ifdef ESP_PLATFORM
    bb_system_restart_reason_at(BB_RESET_SRC_API_REBOOT, detail[0] ? detail : NULL, ts);
#endif /* ESP_PLATFORM */
    return rc;
}

// ---------------------------------------------------------------------------
// Route descriptors
// ---------------------------------------------------------------------------

static const bb_route_response_t s_reboot_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "reboot acknowledged" },
    { 0 },
};

static const bb_route_t s_reboot_route = {
    .method   = BB_HTTP_POST,
    .path     = "/api/reboot",
    .tag      = "system",
    .summary  = "Reboot the device",
    .responses = s_reboot_responses,
    .handler  = reboot_handler,
};

bb_err_t bb_system_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t rc = bb_http_register_described_route(server, &s_reboot_route);
    if (rc != BB_OK) return rc;
    return BB_OK;
}

bb_err_t bb_system_routes_reserve(void)
{
    bb_http_reserve_routes(1);  // POST /api/reboot
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Testing exposure (host unit tests)
// ---------------------------------------------------------------------------

#ifdef BB_SYSTEM_TESTING
bb_err_t bb_system_reboot_handler_for_test(bb_http_request_t *req)
{
    return reboot_handler(req);
}
#endif /* BB_SYSTEM_TESTING */
