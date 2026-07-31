#include "bb_wifi_http.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bb_http.h"
#include "bb_http_body.h"
#include "bb_http_serialize_error.h"
#include "bb_http_serialize_stream.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_openapi.h"
#include "bb_serialize.h"

#include "esp_wifi.h"

// GET /api/wifi wire descriptor (B1-1057) -- private header, not under
// include/ (mirrors bb_health.c's relative include of
// bb_health_wire_priv.h).
#include "../../../components/bb_wifi_http/bb_wifi_http_wire_priv.h"

// POST /api/wifi/scan wire descriptor -- migration of scan_handler's
// bb_json_t-based emitter to a bb_serialize descriptor. Private header,
// same relative-include convention as bb_wifi_http_wire_priv.h above.
#include "../../../components/bb_wifi_http/bb_wifi_http_scan_wire_priv.h"

// PATCH /api/wifi request wire descriptor (B1-1178) -- private header,
// same relative-include convention as bb_wifi_http_wire_priv.h above.
// Portable/unconditional (like the two wire headers above); only its
// CONFIG_BB_WIFI_RECONFIGURE-gated usage below is feature-gated.
#include "../../../components/bb_wifi_http/bb_wifi_http_creds_wire_priv.h"

// PATCH /api/wifi 202 response wire descriptor (B1-1286, firmware-review
// follow-up) -- private header, same relative-include convention as
// bb_wifi_http_wire_priv.h above. Portable/unconditional; only its
// CONFIG_BB_WIFI_RECONFIGURE-gated usage below is feature-gated.
#include "../../../components/bb_wifi_http/bb_wifi_http_patch_status_wire_priv.h"

// Pure bb_err_t -> bb_err_t mapper for a single route-registration attempt
// (B1-1259) -- src/-private header, PRIV_INCLUDE_DIRS "src" (same convention
// as bb_wifi_http_apply_status.h below).
#include "bb_wifi_http_route_dup_status.h"

// PATCH /api/wifi's scratch-acquire fail-closed branch (B1-1285 host-
// testability follow-up to B1-1287) -- src/-private header, same
// PRIV_INCLUDE_DIRS "src" convention as the two headers above. Extracted so
// this insertion point's real bb_data_scratch_acquire()/bb_http_serialize_
// send_error() call chain is host-testable even though this TU (unconditional
// <esp_wifi.h>) cannot itself be host-compiled -- see that header's own doc
// comment.
#include "bb_wifi_http_patch_scratch.h"

#if CONFIG_BB_WIFI_RECONFIGURE
#include "bb_data.h"
#include "bb_settings.h"
#include "bb_wifi_pending.h"
#include "bb_wifi_http_apply_status.h"

#include <stddef.h>
#endif

// Used by register_route_tolerate_dup() below (unconditionally, B1-1259) and
// the scan-schema degrade log (CONFIG_BB_OPENAPI_RUNTIME_META only).
static const char *TAG = "bb_wifi_http_routes";

// GET /api/wifi: always a live read (B1-1119 -- bb_cache's legacy
// bb_json .serialize slot, and bb_cache_get_serialized() with it, is gone;
// this handler's cache-hit branch had been dormant since the wifi producer's
// removal anyway, since no producer ever populated the "wifi" bb_cache topic
// -- see B1-723, which will restore a producer via bb_data, not this legacy
// serializer path).
static bb_err_t wifi_info_handler(bb_http_request_t *req)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    bb_wifi_http_info_wire_t snap;
    bb_wifi_http_info_wire_fill(&snap, &info);

    return bb_http_serialize_stream(req, &bb_wifi_http_info_wire_desc, &snap);
}

static bb_err_t scan_handler(bb_http_request_t *req)
{
    bb_wifi_http_scan_wire_t snap;
    bb_wifi_http_scan_wire_fill(&snap);

    return bb_http_serialize_stream(req, &bb_wifi_http_scan_wire_desc, &snap);
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

// GET /api/wifi response schema literal -- RELOCATED (B1-1059 emit batch B,
// site B1) into components/bb_wifi_http/bb_wifi_http_wire.c as
// k_wifi_info_schema, served here via bb_wifi_http_info_wire_get_schema()
// (config OFF returns that same literal unchanged; config ON returns a
// runtime-composed buffer -- see that fn's own doc comment).

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

// CONFIG_BB_OPENAPI_RUNTIME_META (B1-1059 emit batch B, site B2) -- gated
// DIRECTLY on this Kconfig symbol, same posture as every other exemplar
// (see bb_wifi_routes_init() below). Config OFF (default) is a zero-diff
// no-op: the `#else` arm below is byte-identical to the pre-batch table,
// using the SAME BB_WIFI_HTTP_SCAN_SCHEMA_LITERAL text
// bb_wifi_http_scan_wire.c's config-OFF k_wifi_http_scan_schema uses (see
// bb_wifi_http_scan_wire_priv.h).
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)

// Mutable (`.data`, not `.rodata`) with this config on -- `.schema` starts
// NULL and is patched in once by bb_wifi_routes_init() before this route is
// ever registered/served.
static bb_route_response_t s_scan_responses[] = {
    { 200, "application/json", NULL /* patched at init */,
      "visible access points, wrapped in an aps array" },
    { 0 },
};

#else

static const bb_route_response_t s_scan_responses[] = {
    { 200, "application/json",
      BB_WIFI_HTTP_SCAN_SCHEMA_LITERAL,
      "visible access points, wrapped in an aps array" },
    { 0 },
};

#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

static const bb_route_t s_scan_route = {
    .method   = BB_HTTP_POST,
    .path     = "/api/wifi/scan",
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
// on its own live bb_wifi_get_info() read unchanged; this binding exists
// solely for the apply (ingress) half.
// ---------------------------------------------------------------------------

// Wire type + descriptor (bb_wifi_http_creds_wire_t /
// bb_wifi_http_creds_wire_desc) extracted (B1-1178) into
// components/bb_wifi_http/bb_wifi_http_creds_wire.c -- portable, so it can
// host a BB_SERIALIZE_META_HOST meta table there and link on host (this TU
// is ESP-IDF-only, includes <esp_wifi.h> unconditionally). See
// bb_wifi_http_creds_wire_priv.h for the buffer-sizing rationale (Fork 1,
// B1-1022, user-decided).

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
    memset(dst, 0, sizeof(bb_wifi_http_creds_wire_t));
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
    const bb_wifi_http_creds_wire_t *creds = (const bb_wifi_http_creds_wire_t *)snap;

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

// Request body cap -- MAX BODY BYTES (see bb_http_req_recv_body_stack()'s
// cap-semantics doc); the buffer itself (bb_data's shared scratch pair, see
// bb_data_scratch_acquire()) is sized WIFI_PATCH_BODY_MAX + 1.
#define WIFI_PATCH_BODY_MAX 256

// PATCH /api/wifi 202 response: a single fixed literal field,
// {"status":"rebooting_to_try_wifi"}; migration target for
// wifi_patch_handler's success-path emission below. Wire type + descriptor
// (bb_wifi_http_patch_status_wire_t / bb_wifi_http_patch_status_wire_desc)
// extracted (firmware-review follow-up) into
// components/bb_wifi_http/bb_wifi_http_patch_status_wire.c -- portable, so
// a host fidelity test can drive the real production descriptor instead of
// hand-copying its shape (mirrors bb_wifi_http_creds_wire.c's request-side
// precedent, B1-1178; see bb_wifi_http_patch_status_wire_priv.h). The 400/
// 500 error bodies below it are single-field-error-body shapes too,
// migrated onto the shared bb_http_serialize_send_error()
// (bb_http_serialize_error.h) instead -- see that fn's own doc comment for
// why this common shape is factored centrally rather than getting its own
// per-file descriptor here.

static bb_err_t wifi_patch_handler(bb_http_request_t *req)
{
    // B1-1287 (httpd worker task stack-exhaustion fix): body/parse_scratch
    // now borrow bb_data's shared scratch pair instead of this handler's own
    // `char body[257]; char parse_scratch[3072];` stack locals -- see
    // bb_data.h's own doc comment for the concurrency invariant this relies
    // on (one httpd worker task, one synchronous handler dispatched at a
    // time). The acquire-or-fail-closed-500 sequence itself is extracted
    // into bb_wifi_http_patch_acquire_scratch() (B1-1285) so this insertion
    // point is host-testable -- see bb_wifi_http_patch_scratch.h.
    bb_data_scratch_t scratch;
    bb_err_t scratch_rc = bb_wifi_http_patch_acquire_scratch(req, &scratch);
    if (scratch_rc != BB_OK) {
        return scratch_rc;
    }

    size_t n = 0;
    if (bb_http_req_recv_body_stack(req, scratch.body, WIFI_PATCH_BODY_MAX + 1, &n) != BB_OK) {
        bb_data_scratch_release();
        bb_http_serialize_send_error(req, 400, "missing, oversized, or unreadable body");
        return BB_ERR_INVALID_ARG;
    }

    bb_wifi_http_creds_wire_t dst_scratch;
    bb_data_apply_req_t apply_req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "wifi",
        .mode              = BB_DATA_APPLY_POST,
        .body              = scratch.body,
        .body_len          = n,
        .parse_scratch     = scratch.parse_scratch,
        .parse_scratch_cap = scratch.parse_scratch_cap,
        .dst_scratch       = &dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };
    bb_err_t rc = bb_data_apply(&apply_req);
    bb_data_scratch_release();

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
        bb_http_serialize_send_error(req, 400, "invalid request body or credentials");
        return rc;
    }
    if (status != 202) {
        bb_http_serialize_send_error(req, 500, "internal error");
        return rc;
    }

    bb_http_resp_set_status(req, 202);
    bb_wifi_http_patch_status_wire_t status_snap;
    memset(&status_snap, 0, sizeof(status_snap));
    strncpy(status_snap.status, "rebooting_to_try_wifi", sizeof(status_snap.status) - 1);
    return bb_http_serialize_stream(req, &bb_wifi_http_patch_status_wire_desc, &status_snap);
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

// Register a single described route, tolerating a duplicate (method,path)
// registration as a benign outcome (B1-1259): all three routes registered
// below share the "/api/*" (method,path) space with bb_wifi_prov's own scan
// route (see bb_wifi_prov.c's dup handling for the precedent this mirrors),
// and first-registration-wins is a legitimate composition outcome, not a
// per-route failure. Today bb_http_register_route() already swallows a
// non-strict "/api/*" duplicate to BB_OK internally before it ever reaches
// this caller (B1-1253) -- so the BB_ERR_INVALID_STATE branch logged below
// is defensive/currently-dead in that specific sense, same as
// bb_wifi_prov.c's own dup branch (see its comment). It is kept anyway so
// this fn stays correct and self-documenting if that lower-layer swallow is
// ever tightened back to propagating BB_ERR_INVALID_STATE to its callers,
// and the decision itself is pinned host-side against
// bb_wifi_http_route_register_outcome() (bb_wifi_http_route_dup_status.h),
// since this fn's own caller chain is ESP_PLATFORM-gated and not host-
// testable directly.
//
// A GENUINE per-route failure (e.g. BB_ERR_NO_SPACE — the dispatch table is
// full) is deliberately NOT downgraded here: it still aborts the whole
// bb_wifi_routes_init() bundle below (rc != BB_OK short-circuits the
// caller). A partially-registered wifi HTTP surface (e.g. GET /api/wifi
// present but POST /api/wifi/scan silently missing) is worse than a loud,
// early failure that a consumer's boot sequence can detect and react to —
// the same posture bb_http_register_route() itself already takes for a
// genuine dispatch-table failure (see its own comment). Only the tolerated
// duplicate case is downgraded; every other non-BB_OK stays fatal.
static bb_err_t register_route_tolerate_dup(bb_http_handle_t server, const bb_route_t *route)
{
    bb_err_t rc = bb_http_register_described_route(server, route);
    if (rc == BB_ERR_INVALID_STATE) {
        bb_log_i(TAG, "%s already served by another component", route->path);
    }
    return bb_wifi_http_route_register_outcome(rc);
}

bb_err_t bb_wifi_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    // Route registration ordered AHEAD of the fatal info-schema compose
    // below (B1-1231 fix, mirrors bb_ota_validator_init()'s
    // s_mark_valid_route-before-partitions-compose shape): the info-schema
    // check stays fatal-abort (see its own doc comment further down), but a
    // WifiInfo-schema compose failure and this fn's route registration are
    // unrelated concerns sharing one buffer engine -- the fatal check no
    // longer gets to take down GET /api/wifi, POST /api/wifi/scan, or (with
    // CONFIG_BB_WIFI_RECONFIGURE) PATCH /api/wifi before they're ever
    // registered.
    //
    // Each registration below goes through register_route_tolerate_dup()
    // (B1-1259), not bb_http_register_described_route() directly: a
    // tolerable duplicate for ONE route must not abort registration of the
    // others.
    bb_err_t rc;
    rc = register_route_tolerate_dup(server, &s_wifi_route);
    if (rc != BB_OK) return rc;

    // Compose (config ON only) before registering the scan route. The
    // accessor below is ALWAYS declared (site B1/B2 wire.c pattern, see
    // bb_wifi_http_wire_priv.h's doc comment) and already returns the right
    // content for config OFF, so only the *_ensure_schema_patched() call and
    // the scan response patch are gated. A compose failure here degrades
    // (logs and leaves s_scan_responses[0].schema NULL, already gated by
    // bb_openapi_emit.c's `if (r->schema)`) rather than aborting -- see the
    // commit introducing this degrade for the full rationale.
    //
    // Coverage gap (B1-1231): this branch is not exercised by any host
    // test -- not via bb_wifi_routes_init(), which is ESP_PLATFORM-gated
    // and not host-testable.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    if (bb_wifi_http_scan_wire_ensure_schema_patched() != BB_OK) {
        bb_log_w(TAG, "wifi scan schema compose failed, POST /api/wifi/scan ships undocumented");
    } else {
        s_scan_responses[0].schema = bb_wifi_http_scan_wire_get_schema();
    }
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

    rc = register_route_tolerate_dup(server, &s_scan_route);
    if (rc != BB_OK) return rc;

#if CONFIG_BB_WIFI_RECONFIGURE
    bb_data_binding_t wifi_creds_binding = {
        .key    = "wifi",
        .desc   = &bb_wifi_http_creds_wire_desc,
        .gather = wifi_creds_gather,
        .apply  = wifi_creds_apply,
    };
    rc = bb_data_bind(&wifi_creds_binding);
    if (rc != BB_OK) return rc;
    rc = register_route_tolerate_dup(server, &s_wifi_patch_route);
    if (rc != BB_OK) return rc;
#endif

    // WifiInfo component schema: compose (config ON only) then register.
    // info_schema_rc stays fatal-abort (NOT converted to degrade-and-continue,
    // unlike the scan arm above): bb_wifi_http_info_wire_get_schema() backs
    // bb_openapi_register_schema("WifiInfo", ..., NULL) below, which binds
    // directly to s_wifi_info_schema_buf's static address
    // (bb_wifi_http_wire.c) -- that pointer is non-NULL even when the
    // compose step never ran, so an unpatched buffer would register an
    // EMPTY "WifiInfo" component schema instead of being cleanly omitted
    // (unlike s_scan_responses[0].schema/s_partitions_responses[0].schema,
    // which start NULL and are gated by bb_openapi_emit.c's `if (r->schema)`
    // route-response check). Converting this arm needs a NULL-then-patch
    // rework of bb_wifi_http_wire.c first -- left fatal here deliberately;
    // see B1-1231 follow-up. Deliberately last in this fn (not first, as it
    // was pre-B1-1231): all three routes above are already registered and
    // serving by this point, so this fatal return can no longer take the
    // whole wifi HTTP surface down with it -- only the OpenAPI document's
    // WifiInfo component schema (and this fn's own success return) are at
    // stake.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    bb_err_t info_schema_rc = bb_wifi_http_info_wire_ensure_schema_patched();
    if (info_schema_rc != BB_OK) return info_schema_rc;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
    bb_openapi_register_schema("WifiInfo", bb_wifi_http_info_wire_get_schema(), NULL);

    return BB_OK;
}

bb_err_t bb_wifi_routes_reserve(void)
{
#if CONFIG_BB_WIFI_RECONFIGURE
    bb_http_reserve_routes(3);  // GET /api/wifi + POST /api/wifi/scan + PATCH /api/wifi
#else
    bb_http_reserve_routes(2);  // GET /api/wifi + POST /api/wifi/scan
#endif
    return BB_OK;
}
