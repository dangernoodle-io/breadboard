#include "bb_wifi_http.h"
#include "bb_cache.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bb_http.h"
#include "bb_http_body.h"
#include "bb_http_serialize_stream.h"
#include "bb_http_server.h"
#include "bb_json.h"
#include "bb_openapi.h"
#include "bb_serialize.h"

#include "esp_wifi.h"

// GET /api/wifi wire descriptor (B1-1057) -- private header, not under
// include/ (mirrors bb_health.c's relative include of
// bb_health_wire_priv.h).
#include "../../../components/bb_wifi_http/bb_wifi_http_wire_priv.h"

#if CONFIG_BB_WIFI_RECONFIGURE
#include "bb_data.h"
#include "bb_settings.h"
#include "bb_wifi_pending.h"
#include "bb_wifi_http_apply_status.h"

#include <stddef.h>
#endif

// Local buffer for the memoized /api/wifi payload copy-out.  The wifi section
// has ~10 short fields plus ts_ms; 512 bytes is generous headroom.
#define WIFI_INFO_BUF_BYTES 512

// Serve the memoized telemetry snapshot from bb_cache (SSOT: no re-gather, no
// re-serialize — these are the exact bytes SSE and sinks delivered).  The bytes
// are COPIED out under the cache entry lock into a local buffer (UAF-safe vs a
// concurrent sampler re-serialize).  Falls back to a live read on cache miss.
static bb_err_t wifi_info_handler(bb_http_request_t *req)
{
    char   json[WIFI_INFO_BUF_BYTES];
    size_t len = 0;
    bb_err_t rc = bb_cache_get_serialized(BB_TOPIC_WIFI, json, sizeof(json), &len);
    // NOTE: this cache-hit branch is intentionally dormant since the wifi
    // producer's removal — no producer currently populates the "wifi" bb_cache topic, so
    // every request falls through to the live-read path below. Kept in place
    // for B1-723, which will restore a producer (a bb_meminfo-style wifi
    // snapshot) rather than re-adding pub-captive-sink DI glue.
    if (rc == BB_OK) {
        bb_err_t err = bb_http_resp_set_type(req, "application/json");
        if (err == BB_OK) err = bb_http_resp_send_chunk(req, json, (int)len);
        if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
        return err;
    }

    // Cache miss (pre-first-tick or not registered): fall back to live read.
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    bb_wifi_http_info_wire_t snap;
    bb_wifi_http_info_wire_fill(&snap, &info);

    return bb_http_serialize_stream(req, &bb_wifi_http_info_wire_desc, &snap);
}

static bb_err_t scan_handler(bb_http_request_t *req)
{
    bb_wifi_scan_start_async();

    bb_wifi_ap_t aps[WIFI_SCAN_MAX];
    memset(aps, 0, sizeof(aps));
    int count = bb_wifi_scan_get_cached(aps, WIFI_SCAN_MAX);

    bb_http_json_stream_t stream;
    bb_err_t rc = bb_http_resp_json_arr_begin(req, &stream);
    if (rc != BB_OK) return rc;
    for (int i = 0; i < count; i++) {
        bb_json_t ap = bb_json_obj_new();
        bb_json_obj_set_string(ap, "ssid",   aps[i].ssid);
        bb_json_obj_set_number(ap, "rssi",   aps[i].rssi);
        bb_json_obj_set_bool(ap,   "secure", aps[i].secure);
        bb_http_resp_json_arr_emit(&stream, ap);
        bb_json_free(ap);
    }
    return bb_http_resp_json_arr_end(&stream);
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const char k_wifi_info_schema[] =
    "{\"title\":\"WifiInfo\",\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"string\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"restart_sta_count\":{\"type\":\"integer\"},"
    "\"disconnect_rssi\":{\"type\":\"integer\"}},"
    "\"required\":[\"ssid\",\"connected\"]}";

static const bb_route_response_t s_wifi_responses[] = {
    { 200, "application/json",
      "{\"$ref\":\"#/components/schemas/WifiInfo\"}",
      "current Wi-Fi connection info" },
    { 0 },
};

static const bb_route_t s_wifi_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/wifi",
    .tag      = BB_TOPIC_WIFI,
    .summary  = "Get Wi-Fi connection info",
    .responses = s_wifi_responses,
    .handler  = wifi_info_handler,
};

static const bb_route_response_t s_scan_responses[] = {
    { 200, "application/json",
      "{\"type\":\"array\","
      "\"items\":{"
      "\"type\":\"object\","
      "\"properties\":{"
      "\"ssid\":{\"type\":\"string\"},"
      "\"rssi\":{\"type\":\"integer\"},"
      "\"secure\":{\"type\":\"boolean\"}},"
      "\"required\":[\"ssid\",\"rssi\",\"secure\"]}}",
      "list of visible access points" },
    { 0 },
};

static const bb_route_t s_scan_route = {
    .method   = BB_HTTP_POST,
    .path     = "/api/scan",
    .tag      = BB_TOPIC_WIFI,
    .summary  = "Trigger Wi-Fi network scan and return cached results",
    .responses = s_scan_responses,
    .handler  = scan_handler,
};

// GET /api/diag/wifi moved to the generic bb_diag section registry
// (B1-1077 PR-3a) -- see bb_wifi_http_diag.h's bb_wifi_http_diag_register().

#if CONFIG_BB_WIFI_RECONFIGURE

// ---------------------------------------------------------------------------
// "wifi" bb_data ingress binding (B1-1022) -- backs PATCH /api/wifi via
// bb_data_apply(). Not render-bound to anything: GET /api/wifi above stays
// on its own bb_cache/bb_wifi_get_info() path unchanged; this binding exists
// solely for the apply (ingress) half.
// ---------------------------------------------------------------------------

// Fork 1 (B1-1022, user-decided): bb_serialize_populate()'s get_str
// SILENTLY TRUNCATES a value that overflows a field's max_len -- this
// route's pre-cutover handler instead REJECTED an oversized ssid/pass with
// a 400. To preserve that reject-UX without changing populate's shipped
// truncate contract, ssid/pass here are given MORE buffer than the real
// BB_WIFI_PENDING_SSID_MAX/PASS_MAX limits, so an oversized value survives
// intact (or, past this buffer's own cap, is detectably "buffer-full")
// rather than being silently clipped down to exactly the real limit --
// wifi_creds_apply() then re-checks against the real limit via
// bb_wifi_pending_validate_buf() and rejects. See that fn's own doc for the
// full truncation-safety argument.
#define WIFI_CREDS_APPLY_SSID_BUF (BB_WIFI_PENDING_SSID_MAX + 1 + 32)  // 64
#define WIFI_CREDS_APPLY_PASS_BUF (BB_WIFI_PENDING_PASS_MAX + 1 + 32)  // 96

typedef struct {
    char ssid[WIFI_CREDS_APPLY_SSID_BUF];
    char pass[WIFI_CREDS_APPLY_PASS_BUF];
} bb_wifi_creds_apply_t;

static const bb_serialize_field_t s_wifi_creds_fields[] = {
    { .key = "ssid", .type = BB_TYPE_STR, .offset = offsetof(bb_wifi_creds_apply_t, ssid),
      .max_len = sizeof(((bb_wifi_creds_apply_t *)0)->ssid) },
    { .key = "password", .type = BB_TYPE_STR, .offset = offsetof(bb_wifi_creds_apply_t, pass),
      .max_len = sizeof(((bb_wifi_creds_apply_t *)0)->pass) },
};

static const bb_serialize_desc_t s_wifi_creds_desc = {
    .type_name = "bb_wifi_creds_apply_t",
    .fields    = s_wifi_creds_fields,
    .n_fields  = 2,
    .snap_size = sizeof(bb_wifi_creds_apply_t),
};

// Review fix (B1-1022 finding #1, MEDIUM): this route applies via
// BB_DATA_APPLY_POST (memset0 seed), NOT BB_DATA_APPLY_PATCH. An earlier
// version seeded dst from the currently-staged pending creds
// (bb_settings_wifi_pending_*_get()) so an omitted "password" would keep the
// last-staged password -- but that staged value can belong to a DIFFERENT,
// previously-abandoned ssid, so PATCH {"ssid":"B"} with no password silently
// reused a password staged for ssid "A". POST mode restores the old
// hand-rolled handler's exact behavior instead: an omitted password becomes
// empty/open, an omitted or empty ssid is rejected ("ssid required") by
// wifi_creds_apply()'s validation below. This gather hook is consequently
// UNUSED by the current apply wiring (POST mode never calls it) -- it exists
// only to satisfy bb_data_bind()'s non-NULL-gather invariant (a binding with
// no gather is rejected outright) and stands ready for a future GET-path
// render of this same key, should one ever bind to it.
static bb_err_t wifi_creds_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    memset(dst, 0, sizeof(bb_wifi_creds_apply_t));
    return BB_OK;
}

// Ingress hook: re-validates (Fork 1 oversize-reject + non-empty ssid, via
// bb_wifi_pending_validate_buf()) then stages the reconfigure -- the
// downstream commit (bb_wifi_reconfigure -> bb_settings_wifi_pending_set,
// deferred-reboot timer) is UNCHANGED from the pre-cutover handler.
// bb_wifi_reconfigure()'s own return is intentionally not surfaced as a
// distinct error class here (same fire-and-forget posture the old handler
// had once validation passed) -- any of its own internal failures fold into
// this fn's return.
static bb_err_t wifi_creds_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    const bb_wifi_creds_apply_t *creds = (const bb_wifi_creds_apply_t *)snap;

    if (bb_wifi_pending_validate_buf(creds->ssid, sizeof(creds->ssid),
                                      creds->pass, sizeof(creds->pass)) != BB_OK) {
        return BB_ERR_VALIDATION;
    }

    // bb_wifi_reconfigure() re-runs bb_wifi_pending_validate() (strlen-based)
    // on the same creds internally -- intentional defense-in-depth, not
    // redundant dead work: it's safe because validate_buf above already
    // proved both buffers are NUL-terminated within their real limits, so
    // the strlen() path downstream cannot run unbounded.
    return bb_wifi_reconfigure(creds->ssid, creds->pass);
}

// JSON parse scratch: a flat 2-string-field document, comfortably under the
// route's own 256-byte body cap -- but the token recorder's own
// default-capacity pool alone is 48 * sizeof(bb_serialize_json_tok_t) ==
// 2304 bytes, so this must clear that plus the control structs plus
// headroom for the escape-decode arena (see
// test_bb_serialize_json_parse.c's SCRATCH_CAP comment for the full
// control-struct/token-pool/escape-arena layout this backs). Sized to match
// bb_diag_section's own precedent for a multi-KB stack scratch buffer in an
// httpd handler call chain (BB_DIAG_SECTION_RENDER_BUF_BYTES, default
// 3072) -- this route's own handler-stack task budget is
// CONFIG_BB_HTTP_TASK_STACK_SIZE (default 6144).
#define WIFI_PATCH_PARSE_SCRATCH_BYTES 3072

// Request body cap -- MAX BODY BYTES (see bb_http_req_recv_body_stack()'s
// cap-semantics doc); the stack buffer itself is sized
// WIFI_PATCH_BODY_MAX + 1.
#define WIFI_PATCH_BODY_MAX 256

static bb_err_t wifi_patch_handler(bb_http_request_t *req)
{
    char   body[WIFI_PATCH_BODY_MAX + 1];
    size_t n = 0;
    if (bb_http_req_recv_body_stack(req, body, sizeof(body), &n) != BB_OK) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "missing, oversized, or unreadable body");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    bb_wifi_creds_apply_t dst_scratch;
    char                  parse_scratch[WIFI_PATCH_PARSE_SCRATCH_BYTES];
    bb_data_apply_req_t apply_req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "wifi",
        .mode              = BB_DATA_APPLY_POST,
        .body              = body,
        .body_len          = n,
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = &dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };
    bb_err_t rc = bb_data_apply(&apply_req);

    // Fork 3 (B1-1022, user-decided): response shaping (incl. this 202 body)
    // stays here in the route -- bb_data_apply()/apply() stay HTTP-agnostic
    // and never see a status code, only a bb_err_t.
    //
    // Review fix (B1-1022 finding #3, LOW): BB_ERR_NOT_FOUND (no "wifi"
    // binding) is deliberately NOT in this 400 list -- it signals a
    // composition-invariant violation (the binding is registered a few lines
    // below in this same fn, before this route is ever reachable), not a
    // client error, so it falls through to the 500 branch below.
    //
    // BB_ERR_VALIDATION stays in this list: wifi_creds_apply() (the apply()
    // hook above) returns it for a domain reject (bad ssid/password), which
    // is a 400 same as a parse failure. BB_ERR_PARSE_GRAMMAR/
    // BB_ERR_PARSE_INCOMPLETE cover the disjoint parse-layer namespace (see
    // bb_core.h) -- a malformed or truncated request body from
    // bb_data_apply()'s parse() call, previously mismapped to
    // BB_ERR_INVALID_STATE and wrongly falling through to the 500 branch.
    //
    // The mapping itself lives in bb_wifi_http_apply_status.c (host-testable
    // pure fn) so the pinning tests in test_wifi_creds_apply_route.c exercise
    // the exact production list, not a hand-copy of it.
    int status = bb_wifi_http_status_for_apply_rc(rc);
    if (status == 400) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid request body or credentials");
        bb_http_resp_json_obj_end(&obj);
        return rc;
    }
    if (status != 202) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "internal error");
        bb_http_resp_json_obj_end(&obj);
        return rc;
    }

    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting_to_try_wifi");
    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_wifi_patch_responses[] = {
    { 202, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "reconfigure accepted; device will reboot to try new credentials" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "invalid request body or credentials" },
    { 0 },
};

static const bb_route_t s_wifi_patch_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/wifi",
    .tag                  = BB_TOPIC_WIFI,
    .summary              = "Stage new Wi-Fi credentials and arm deferred reboot",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\","
                            "\"properties\":{\"ssid\":{\"type\":\"string\",\"maxLength\":31},"
                            "\"password\":{\"type\":\"string\",\"maxLength\":63}},"
                            "\"required\":[\"ssid\"]}",
    .responses            = s_wifi_patch_responses,
    .handler              = wifi_patch_handler,
};

#endif /* CONFIG_BB_WIFI_RECONFIGURE */

bb_err_t bb_wifi_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_openapi_register_schema("WifiInfo", k_wifi_info_schema, NULL);
    bb_err_t rc;
    rc = bb_http_register_described_route(server, &s_wifi_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_scan_route);
    if (rc != BB_OK) return rc;
#if CONFIG_BB_WIFI_RECONFIGURE
    bb_data_binding_t wifi_creds_binding = {
        .key    = "wifi",
        .desc   = &s_wifi_creds_desc,
        .gather = wifi_creds_gather,
        .apply  = wifi_creds_apply,
    };
    rc = bb_data_bind(&wifi_creds_binding);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_wifi_patch_route);
    if (rc != BB_OK) return rc;
#endif
    return BB_OK;
}

bb_err_t bb_wifi_routes_reserve(void)
{
#if CONFIG_BB_WIFI_RECONFIGURE
    bb_http_reserve_routes(3);  // GET /api/wifi + POST /api/scan + PATCH /api/wifi
#else
    bb_http_reserve_routes(2);  // GET /api/wifi + POST /api/scan
#endif
    return BB_OK;
}
