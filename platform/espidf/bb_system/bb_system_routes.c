// bb_system_routes — POST /api/reboot handler (B1-1148 PR2).
//
// Portable: no ESP-IDF-specific includes at the top level. Compiled on both
// ESP-IDF and host (for unit tests) -- same posture as
// bb_storage_http_routes.c, which this file mirrors. The one on-device-only
// call, bb_system_restart_reason_at(), is gated behind #ifdef ESP_PLATFORM
// below: its host stub (platform/host/bb_system/bb_system_host.c) calls
// exit(0), which would kill the host test process if reached unconditionally.
//
// PR2 (B1-1148) migrates the request body off the hand-rolled
// bb_system_reboot_parse_body() (deleted -- see bb_system_reboot_parse.c's
// git history) onto a "reboot" bb_data ingress binding, driven through
// bb_data_apply() -- the same template bb_storage_http_routes.c's
// factory_reset_handler and bb_wifi_http_routes.c's wifi_patch_handler use.
// Both fields ("ts", "detail") stay optional; the binding's apply() hook's
// ONLY domain check is the ts divergence guard (see reboot_apply()'s doc
// comment below) -- the User-Agent merge, the ts clamp, and the on-device
// restart all still happen HERE in the route, reading the same dst_scratch
// buffer the route itself supplied to bb_data_apply(), after that call
// returns.
//
// BREAKING CHANGE (B1-1148, user-approved): a malformed (grammar-invalid or
// truncated) JSON body now returns 400. Previously it was silently
// tolerated (200, ts=0, detail falls back to the User-Agent header) --
// same posture as every other bb_data-fed route in this codebase
// (factory_reset_handler, wifi_patch_handler). An ABSENT or EMPTY body is
// UNCHANGED: still tolerated (200), never reaches bb_data_apply() at all
// (see body_len > 0 gate below) -- only a body that was actually sent and
// fails to parse, OR a "ts" that isn't a clean base-10 integer (see
// reboot_apply()'s doc comment), is newly rejected.

#include "bb_data.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_serialize.h"
#include "bb_system.h"

#ifdef BB_SYSTEM_TESTING
#include "bb_system_test.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Bound on the request body accepted for POST /api/reboot's optional
// {"ts": <epoch_s>, "detail": "<string>"} JSON. Both fields are optional;
// no body at all is a normal, tolerated request.
#define BB_SYSTEM_REBOOT_BODY_MAX 256

// JSON parse scratch: a flat 2-field document, comfortably under the
// route's own 256-byte body cap -- but the token recorder's own
// default-capacity pool alone is 48 * sizeof(bb_serialize_json_tok_t) ==
// 2304 bytes, so this must clear that plus the control structs plus
// headroom for the escape-decode arena regardless of body size (same
// fixed-pool-size rationale as bb_wifi_http_routes.c's
// WIFI_PATCH_PARSE_SCRATCH_BYTES / bb_storage_http_routes.c's
// FACTORY_RESET_PARSE_SCRATCH_BYTES).
#define BB_SYSTEM_REBOOT_PARSE_SCRATCH_BYTES 3072

// ---------------------------------------------------------------------------
// "reboot" bb_data ingress binding (B1-1148 PR2) -- backs POST /api/reboot's
// optional {"ts": <epoch_s>, "detail": "<string, up to 48 chars>"} body.
// ---------------------------------------------------------------------------

typedef struct {
    // BB_TYPE_U64's populate path does an 8-byte memcpy into this field --
    // MUST stay uint64_t (not uint32_t, which would overflow into adjacent
    // struct memory). The route clamps the resolved value down to
    // uint32_t by hand after bb_data_apply() returns (see reboot_handler
    // below) -- same (0, UINT32_MAX] range the deleted
    // bb_system_reboot_parse_body() enforced.
    uint64_t ts;
    // ts divergence guard (B1-1148 finding 1, user-approved) -- a SECOND
    // field bound to the SAME "ts" wire key, but as BB_TYPE_F64. `ts` above
    // resolves via bb_serialize_json_tok_get_i64() (BB_TYPE_U64's populate
    // path, which is built on get_i64()); this field resolves the identical
    // raw number text via bb_serialize_json_tok_get_f64() (strtod(), which
    // always honors the full JSON number grammar -- fraction and exponent
    // -- regardless of the B1-1164 refusal below).
    //
    // Post-B1-1164, bb_serialize_json_tok_get_i64() itself now REFUSES
    // (returns false) whenever "ts"'s raw text has a fraction or exponent,
    // rather than silently truncating it -- see
    // bb_serialize_json_tok.c:tok_parse_num()'s doc comment. A refused
    // field is, to BB_TYPE_U64's populate arm, indistinguishable from an
    // ABSENT one: both leave `ts` untouched at its BB_DATA_APPLY_POST
    // memset0 seed (0). Without this shadow field, reboot_apply() could
    // never tell "ts" was absent (a normal, tolerated request -- 200, ts=0)
    // apart from "ts" was present but refused (a malformed timestamp --
    // must 400, not silently resolve to 0). get_f64() never refuses, so
    // ts_f64 stays 0.0 in the absent case but picks up the real (nonzero,
    // or occasionally coincidentally-zero) value in the refused case --
    // reboot_apply() below compares the two to tell them apart. This field
    // is not itself surfaced to any consumer; it exists purely for that
    // comparison.
    double ts_f64;
    // max_len below is sizeof(detail) - 1, NOT sizeof(detail) -- populate's
    // get_str() copies min(value_len, max_len) bytes and does NOT reserve a
    // byte for the NUL terminator itself; leaving one byte of headroom here
    // is what keeps this buffer NUL-terminated after the widest possible
    // scatter (dst_scratch is memset0-seeded by BB_DATA_APPLY_POST, so the
    // reserved trailing byte is already '\0').
    char detail[49];
} bb_system_reboot_apply_t;

// The "ts" key is intentionally bound TWICE below (BB_TYPE_U64 then
// BB_TYPE_F64) -- relies on bb_serialize.h's documented duplicate-`.key`
// contract (see its field-table doc comment): each scalar getter resolves
// independently by first-match, so field order here is irrelevant and no
// cursor state is shared between the two reads. If that contract is ever
// narrowed (e.g. a single forward-only cursor pass, or duplicate-key
// rejection added to populate_check_fields()), this binding breaks
// silently -- see reboot_apply()'s doc comment above for why the second
// "ts" read exists at all.
static const bb_serialize_field_t s_reboot_fields[] = {
    { .key = "ts", .type = BB_TYPE_U64, .offset = offsetof(bb_system_reboot_apply_t, ts) },
    { .key = "ts", .type = BB_TYPE_F64, .offset = offsetof(bb_system_reboot_apply_t, ts_f64) },
    { .key = "detail", .type = BB_TYPE_STR, .offset = offsetof(bb_system_reboot_apply_t, detail),
      .max_len = sizeof(((bb_system_reboot_apply_t *)0)->detail) - 1 },
};

static const bb_serialize_desc_t s_reboot_desc = {
    .type_name = "bb_system_reboot_apply_t",
    .fields    = s_reboot_fields,
    .n_fields  = 3,
    .snap_size = sizeof(bb_system_reboot_apply_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1181a) -- co-located JSON Schema companion to
// s_reboot_desc above, gated behind BB_SERIALIZE_META_HOST (see
// bb_system.h's banner). NOT a oneOf: the wire binds "ts" TWICE (BB_TYPE_U64
// + BB_TYPE_F64 divergence-guard shadow, see s_reboot_fields' doc comment
// above), but the F64 occurrence is an internal check never meant to
// surface -- the hand-authored POST /api/reboot request_schema below
// renders "ts":{"type":"integer"} only. The meta row below tags occurrence
// 0 (the U64 occurrence, kind defaults to BB_SERIALIZE_META_KIND_FIELD) and
// carries no row for occurrence 1 (the F64 shadow), which
// bb_serialize_meta_openapi_schema() then leaves doc-invisible -- see
// test_bb_system_reboot_meta_golden.c for the fidelity proof. Neither field
// is required per s_reboot_route's request_schema (no "required" array at
// all -- see the composer's documented "always-emits-required" delta,
// same as every other exemplar).
// ---------------------------------------------------------------------------
// bb_system.h only pulls in bb_serialize_meta.h nested inside its own
// BB_SYSTEM_TESTING guard (the test-accessor declarations below) -- not
// defined on a real device build with CONFIG_BB_OPENAPI_RUNTIME_META=y, so
// this TU needs its own direct, UNCONDITIONAL include here: BB_SERIALIZE_
// META_SHIP is itself defined INSIDE bb_serialize_meta.h, so checking it
// before ever including that header (the only way to actually get the
// #define) would always read false regardless of the true condition.
#include "bb_serialize_meta.h"

#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_reboot_meta_rows[] = {
    { .key = "ts", .occurrence = 0 },
    { .key = "detail" },
};

const bb_serialize_desc_meta_t bb_system_reboot_meta = {
    .type_name = "bb_system_reboot_apply_t",
    .rows      = s_reboot_meta_rows,
    .n_rows    = sizeof(s_reboot_meta_rows) / sizeof(s_reboot_meta_rows[0]),
};

#ifdef BB_SYSTEM_TESTING
const bb_serialize_desc_t *bb_system_reboot_desc_for_test(void)
{
    return &s_reboot_desc;
}
#endif /* BB_SYSTEM_TESTING */

#endif /* BB_SERIALIZE_META_SHIP */

// Egress hook: exists only to satisfy bb_data_bind()'s non-NULL-gather
// invariant (a binding with no gather is rejected outright) -- this route is
// POST-only, so gather is never actually invoked (BB_DATA_APPLY_POST always
// memset0-seeds dst_scratch, see bb_data_apply()'s doc). Same posture as
// factory_reset_gather() in bb_storage_http_routes.c.
static bb_err_t reboot_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    memset(dst, 0, sizeof(bb_system_reboot_apply_t));
    return BB_OK;
}

// Ingress hook: the User-Agent fallback merge, the ts clamp, and the
// on-device restart all still happen in reboot_handler below, AFTER
// bb_data_apply() returns, reading the same dst_scratch the route itself
// supplied. This hook's ONE domain check: the ts divergence guard
// (B1-1148 finding 1, user-approved).
//
// Post-B1-1164, bb_serialize_json_tok_get_i64() (and so BB_TYPE_U64's
// populate arm, which `ts` above is bound as) REFUSES outright whenever
// "ts"'s raw text has a fraction or exponent ("5e1", "1.5e10", "1e300",
// "1.9" are all grammar-valid JSON numbers, but none is a clean base-10
// integer) -- see tok_parse_num()'s doc comment in bb_serialize_json_tok.c.
// A refused field leaves `ts` at its BB_DATA_APPLY_POST memset0 seed (0),
// same as an absent one -- see ts_f64's own doc comment above for why the
// shadow field is what tells the two apart: absent leaves BOTH `ts` and
// `ts_f64` at 0 (get_f64() never refuses, but there was no number to read
// either way); refused leaves `ts` at 0 while `ts_f64` picks up the real,
// full-grammar value via strtod() -- so (double)ts_i64 != ts_f64 is exactly
// the refused-and-therefore-must-reject signal. Every refused "ts"
// ("5e1", "1.5e10", "1e300", "1.9", or the genuinely dangerous
// "9223372036854775807e-18" that shrinks into an otherwise-plausible
// in-range value) is rejected the same way: BB_ERR_VALIDATION (mapped to
// 400 by reboot_handler), per the user's decision that a "ts" which isn't
// a clean integer the parse can honour exactly must be rejected rather
// than silently resolved to some other value.
//
// The one remaining wrinkle is unrelated to refusal: a genuine, no-
// exponent overflow of a CLEAN digit run (e.g. a 20-digit plain-integer
// "ts") is not refused at all -- BB_TYPE_U64.num_inexact is only set by a
// '.'/'e'/'E' in the raw text, never by magnitude alone -- so get_i64()
// still succeeds, returning strtoll()'s C-standard-mandated saturated
// INT64_MAX/INT64_MIN. That's a real (if extreme) value, not a refusal, so
// it must NOT hit the mismatch check above: strtod()'s ts_f64 for the same
// text is astronomically larger/smaller than the saturated (double)ts_i64
// (e.g. INT64_MAX vs. ~1e20), which would otherwise misfire as a
// divergence. reboot_handler()'s own (0, UINT32_MAX] clamp already reduces
// any such huge value to 0 regardless, matching the deleted double-based
// clamp's behavior for the same input -- so this case is let through here
// unconditionally, before the general mismatch check ever runs.
static bb_err_t reboot_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    const bb_system_reboot_apply_t *s = snap;

    int64_t ts_i64 = (int64_t)s->ts;
    if (ts_i64 == INT64_MAX || ts_i64 == INT64_MIN) {
        return BB_OK;
    }
    if ((double)ts_i64 != s->ts_f64) {
        return BB_ERR_VALIDATION;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Response error helpers
// ---------------------------------------------------------------------------

static void send_400(bb_http_request_t *req, const char *msg)
{
    bb_http_resp_set_status(req, 400);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

static void send_500(bb_http_request_t *req, const char *msg)
{
    bb_http_resp_set_status(req, 500);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Testing capture (host unit tests, B1-1148 finding 1) -- records what
// reboot_handler() actually RESOLVED (detail/ts, after the User-Agent
// fallback) immediately before the #ifdef ESP_PLATFORM restart call below.
// That call is the only consumer of the resolved values and is compiled out
// on host, so without this seam the fallback logic runs but is unobservable
// from host tests. Reused by PR2 for asserting its own parse results.
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
    // Read the (optional) body, bounded and NUL-terminated. An absent body,
    // an empty body, or a body larger than BB_SYSTEM_REBOOT_BODY_MAX are all
    // treated identically (body_len stays 0, bb_data_apply() is never
    // called) -- UNCHANGED from before PR2, the one deliberate posture this
    // migration preserves alongside the new malformed-body 400.
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

    bb_system_reboot_apply_t dst_scratch;
    memset(&dst_scratch, 0, sizeof(dst_scratch));

    if (body_len > 0) {
        char parse_scratch[BB_SYSTEM_REBOOT_PARSE_SCRATCH_BYTES];
        bb_data_apply_req_t apply_req = {
            .fmt               = BB_FORMAT_JSON,
            .key               = "reboot",
            .mode              = BB_DATA_APPLY_POST,
            .body              = body,
            .body_len          = (size_t)body_len,
            .parse_scratch     = parse_scratch,
            .parse_scratch_cap = sizeof(parse_scratch),
            .dst_scratch       = &dst_scratch,
            .dst_scratch_cap   = sizeof(dst_scratch),
        };
        bb_err_t rc = bb_data_apply(&apply_req);

        // BREAKING CHANGE (B1-1148, user-approved): a body that was actually
        // sent but fails to parse (grammar-invalid, e.g. "not-json", or
        // truncated mid-object) now returns 400 -- previously tolerated
        // (200, ts=0, detail falls back to the User-Agent header). Mirrors
        // bb_storage_http_routes.c's factory_reset_handler / bb_wifi_http_
        // routes.c's wifi_patch_handler, the same disjoint parse-layer codes
        // from bb_core.h (B1-1090).
        if (rc == BB_ERR_PARSE_GRAMMAR || rc == BB_ERR_PARSE_INCOMPLETE) {
            send_400(req, "invalid JSON");
            return rc;
        }
        // ts divergence guard (B1-1148 finding 1, user-approved) -- see
        // reboot_apply()'s doc comment. reboot_apply() returns
        // BB_ERR_VALIDATION when "ts"'s raw text isn't a clean base-10
        // integer the strtoll()-backed BB_TYPE_U64 path can honour exactly
        // (a fraction or exponent), same domain-reject posture as
        // bb_storage_http_routes.c's factory_reset_apply().
        if (rc == BB_ERR_VALIDATION) {
            send_400(req, "invalid ts");
            return rc;
        }
        // Every other bb_data_apply() failure here is a composition-
        // invariant violation (the "reboot" binding is bound as
        // apply-capable in bb_system_routes_init()/bb_system_reboot_bind_
        // for_test() before this route is ever reachable) -- not reachable
        // via client input on this route, but handled defensively rather
        // than proceeding with a possibly partially-scattered dst_scratch.
        if (rc != BB_OK) {
            send_500(req, "reboot request processing failed");
            return rc;
        }
    }

    // Resolve the User-Agent fallback once, up front. Precedence: body
    // detail (non-empty) > User-Agent header (non-NULL/non-empty) > "" --
    // same precedence the deleted bb_system_reboot_parse_body() enforced.
    if (!dst_scratch.detail[0]) {
        char ua[49];
        if (bb_http_req_get_header(req, "User-Agent", ua, sizeof(ua)) == BB_OK && ua[0]) {
            strncpy(dst_scratch.detail, ua, sizeof(dst_scratch.detail) - 1);
            dst_scratch.detail[sizeof(dst_scratch.detail) - 1] = '\0';
        }
    }

    // ts clamp: valid range is (0, UINT32_MAX], matching the deleted
    // bb_system_reboot_parse_body()'s double-based clamp
    // (ts_num > 0.0 && ts_num <= (double)UINT32_MAX). dst_scratch.ts is the
    // raw uint64_t BB_TYPE_U64 populated it with -- a negative JSON number
    // (e.g. -5) arrives here as its int64_t bit pattern reinterpreted as
    // uint64_t (populate_get_u64() casts get_i64()'s result), which wraps to
    // a value far above UINT32_MAX and is correctly excluded by the
    // upper-bound check below without any separate sign check.
    uint32_t ts = (dst_scratch.ts > 0 && dst_scratch.ts <= (uint64_t)UINT32_MAX)
                      ? (uint32_t)dst_scratch.ts
                      : 0;

    bb_http_json_obj_stream_t obj;
    bb_err_t rc = bb_http_resp_json_obj_begin(req, &obj);
    if (rc != BB_OK) return rc;
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting");
    rc = bb_http_resp_json_obj_end(&obj);
#ifdef BB_SYSTEM_TESTING
    s_reboot_capture.called = true;
    strncpy(s_reboot_capture.detail, dst_scratch.detail, sizeof(s_reboot_capture.detail) - 1);
    s_reboot_capture.detail[sizeof(s_reboot_capture.detail) - 1] = '\0';
    s_reboot_capture.ts = ts;
#endif /* BB_SYSTEM_TESTING */
#ifdef ESP_PLATFORM
    bb_system_restart_reason_at(BB_RESET_SRC_API_REBOOT, dst_scratch.detail[0] ? dst_scratch.detail : NULL, ts);
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
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "malformed request body (a body that was sent but failed to parse, or "
      "a \"ts\" that isn't a clean integer, e.g. scientific/decimal "
      "notation; an absent or empty body is still accepted)" },
    { 0 },
};

static const bb_route_t s_reboot_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/reboot",
    .tag                  = "system",
    .summary              = "Reboot the device",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\","
                            "\"properties\":{"
                            "\"ts\":{\"type\":\"integer\"},"
                            "\"detail\":{\"type\":\"string\"}}}",
    .responses            = s_reboot_responses,
    .handler              = reboot_handler,
};

bb_err_t bb_system_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_data_binding_t reboot_binding = {
        .key    = "reboot",
        .desc   = &s_reboot_desc,
        .gather = reboot_gather,
        .apply  = reboot_apply,
    };
    bb_err_t rc = bb_data_bind(&reboot_binding);
    if (rc != BB_OK) return rc;

    rc = bb_http_register_described_route(server, &s_reboot_route);
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

// Test-only: binds the "reboot" bb_data key against the production
// gather/apply hooks without going through bb_system_routes_init() (which
// additionally requires a real bb_http_handle_t server -- unavailable in
// host tests). Call after bb_data_test_reset() and before driving
// bb_system_reboot_handler_for_test(). Mirrors bb_storage_http_routes.c's
// bb_storage_http_factory_reset_bind_for_test().
bb_err_t bb_system_reboot_bind_for_test(void)
{
    bb_data_binding_t reboot_binding = {
        .key    = "reboot",
        .desc   = &s_reboot_desc,
        .gather = reboot_gather,
        .apply  = reboot_apply,
    };
    return bb_data_bind(&reboot_binding);
}
#endif /* BB_SYSTEM_TESTING */
