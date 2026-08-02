// bb_system_routes — POST /api/diag/reboot handler (B1-1148 PR2).
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
#include "bb_http_serialize_error.h"
#include "bb_http_serialize_stream.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_serialize.h"
#include "bb_system.h"
#include "bb_system_routes.h"

#ifdef BB_SYSTEM_TESTING
#include "bb_system_test.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Guarded with its sole use site below (the reboot-schema compose warning
// log) -- CONFIG_BB_OPENAPI_RUNTIME_META off leaves TAG otherwise
// unreferenced (same posture as bb_wifi_http_routes.c's TAG guard).
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
static const char *TAG = "bb_system_routes";
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

// Bound on the request body accepted for POST /api/diag/reboot's optional
// {"ts": <epoch_s>, "detail": "<string>"} JSON. Both fields are optional;
// no body at all is a normal, tolerated request.
#define BB_SYSTEM_REBOOT_BODY_MAX 256

// ---------------------------------------------------------------------------
// "reboot" bb_data ingress binding (B1-1148 PR2) -- backs POST /api/diag/reboot's
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
// surface -- the hand-authored POST /api/diag/reboot request_schema below
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
// Response error helpers (B1-1286: migrated off bb_http_json_obj_stream_t's
// 1048-byte stack buffer onto the shared bb_http_serialize_send_error() --
// see its own doc comment in bb_http_serialize_error.h for why this
// stack-overflow-class fix lives there rather than re-hand-rolled here).
// ---------------------------------------------------------------------------

static void send_400(bb_http_request_t *req, const char *msg)
{
    bb_http_serialize_send_error(req, 400, msg);
}

static void send_500(bb_http_request_t *req, const char *msg)
{
    bb_http_serialize_send_error(req, 500, msg);
}

// ---------------------------------------------------------------------------
// POST /api/diag/reboot 200 response wire descriptor (B1-1286) -- a single fixed
// literal field, {"status":"rebooting"}; migration target for
// reboot_handler's success-path emission below (see that call site's own
// doc comment for the ordering guarantee this preserves).
// ---------------------------------------------------------------------------

typedef struct {
    char status[16];
} bb_system_reboot_status_wire_t;

static const bb_serialize_field_t s_reboot_status_wire_fields[] = {
    { .key = "status", .type = BB_TYPE_STR, .offset = offsetof(bb_system_reboot_status_wire_t, status),
      .max_len = sizeof(((bb_system_reboot_status_wire_t *)0)->status) },
};

static const bb_serialize_desc_t s_reboot_status_wire_desc = {
    .type_name = "bb_system_reboot_status_wire_t",
    .fields    = s_reboot_status_wire_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bb_system_reboot_status_wire_t),
};

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
    // B1-1287 (httpd worker task stack-exhaustion fix): body/parse_scratch
    // now borrow bb_data's shared scratch pair (bb_data_scratch_acquire()/
    // _release()) instead of this handler's own `char body[257]; char
    // parse_scratch[3072];` stack locals -- see bb_data.h's own doc comment
    // for the concurrency invariant this relies on (one httpd worker task,
    // one synchronous handler dispatched at a time). Acquired unconditionally
    // up front: this route's body is OPTIONAL, but reading it (even to
    // discover it's absent/oversized) still needs somewhere to receive into.
    bb_data_scratch_t scratch;
    bb_err_t scratch_rc = bb_data_scratch_acquire(&scratch);
    if (scratch_rc != BB_OK) {
        send_500(req, "reboot request processing failed");
        return scratch_rc;
    }

    // Read the (optional) body, bounded and NUL-terminated. An absent body,
    // an empty body, or a body larger than BB_SYSTEM_REBOOT_BODY_MAX are all
    // treated identically (body_len stays 0, bb_data_apply() is never
    // called) -- UNCHANGED from before PR2, the one deliberate posture this
    // migration preserves alongside the new malformed-body 400.
    int  body_len = 0;
    int  raw_len  = bb_http_req_body_len(req);
    if (raw_len > 0 && raw_len <= BB_SYSTEM_REBOOT_BODY_MAX) {
        int n = bb_http_req_recv(req, scratch.body, BB_SYSTEM_REBOOT_BODY_MAX);
        if (n > 0) {
            scratch.body[n] = '\0';
            body_len = n;
        }
    }

    bb_system_reboot_apply_t dst_scratch;
    memset(&dst_scratch, 0, sizeof(dst_scratch));

    if (body_len > 0) {
        bb_data_apply_req_t apply_req = {
            .fmt               = BB_FORMAT_JSON,
            .key               = "reboot",
            .mode              = BB_DATA_APPLY_POST,
            .body              = scratch.body,
            .body_len          = (size_t)body_len,
            .parse_scratch     = scratch.parse_scratch,
            .parse_scratch_cap = scratch.parse_scratch_cap,
            .dst_scratch       = &dst_scratch,
            .dst_scratch_cap   = sizeof(dst_scratch),
        };
        bb_err_t rc = bb_data_apply(&apply_req);
        bb_data_scratch_release();

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
    } else {
        bb_data_scratch_release();
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

    // 200 {"status":"rebooting"} via bb_http_serialize_stream() (B1-1286) --
    // the response is fully flushed/finalized by this call before the
    // ESP_PLATFORM restart below is armed, same ordering guarantee
    // bb_http_resp_json_obj_end() gave the deleted bb_http_json_obj_stream_t
    // path (that ordering, not the emission mechanism, is what a truncated-
    // reply hazard depends on -- see the restart call's own doc comment).
    //
    // Pre-check bb_http_resp_set_type() -- the same call
    // bb_http_serialize_stream() itself makes first internally -- so a
    // Content-Type-set failure (never observed to fail on real hardware;
    // exercised only by the host test's force_set_type_fail hook) returns
    // BEFORE any response byte is written and BEFORE the capture/restart
    // below run, same fail-closed posture bb_http_resp_json_obj_begin()'s
    // failure path gave the deleted bb_http_json_obj_stream_t idiom: if the
    // response can't even be started, arming a restart the client will
    // never see acknowledged is the wrong failure mode. A failure from
    // bb_http_serialize_stream() itself (past this point -- e.g. the
    // chunked-terminator send) still falls through to capture/restart below,
    // same as bb_http_resp_json_obj_end()'s failure path always did.
    bb_err_t rc = bb_http_resp_set_type(req, "application/json");
    if (rc != BB_OK) return rc;

    bb_system_reboot_status_wire_t status_snap;
    memset(&status_snap, 0, sizeof(status_snap));
    strncpy(status_snap.status, "rebooting", sizeof(status_snap.status) - 1);
    rc = bb_http_serialize_stream(req, &s_reboot_status_wire_desc, &status_snap);
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

// A #define (not just the extern variable below) so the runtime-compose
// buffer's size and the config-OFF route table below can both use the SAME
// literal text as a genuine compile-time constant expression -- mirrors
// bb_diag_storage_nvs.c's precedent (B1-1180 PR-1 / B1-1059 pilot).
// Byte-fidelity against the co-located s_reboot_desc/bb_system_reboot_meta
// pair above is proven by test_bb_system_reboot_meta_golden.c (accepted
// delta: the hand literal below has NO "required" key at all; compose adds
// "required":[] (empty) -- neither field is required either way).
#define BB_SYSTEM_REBOOT_REQUEST_SCHEMA_LITERAL \
    "{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"ts\":{\"type\":\"integer\"}," \
    "\"detail\":{\"type\":\"string\"}}}"

const char *const bb_system_reboot_request_schema = BB_SYSTEM_REBOOT_REQUEST_SCHEMA_LITERAL;

// CONFIG_BB_OPENAPI_RUNTIME_META (B1-1059 emit batch A, site 2) -- gated
// DIRECTLY on this Kconfig symbol, never on BB_SERIALIZE_META_SHIP: that
// macro also covers BB_SERIALIZE_META_HOST (unconditionally set by the plain
// `native` host env, see platformio.ini), which must NOT flip this route
// onto the runtime-compose path. Config OFF (default) is a zero-diff no-op:
// the `#else` arm below is byte-identical to the pre-migration inline
// literal.
//
// PROV-GATE HAZARD (B1-1398): this route moved from top-level /api/reboot to
// /api/diag/reboot for namespace consistency with the other administrative
// endpoints under /api/diag/. bb_http_prov_gate's wildcard allowlist entries
// are prefix matches (bb_route_uri_match, see bb_http_prov_gate.h), so a
// consumer that calls bb_http_prov_allow(method, "/api/diag/*") during
// provisioning now ALSO exposes this reboot route -- something a bare
// /api/reboot path never fell under. No in-tree consumer allowlists a
// /api/diag/* (or broader /api/*) prefix today (the sole production caller,
// bb_wifi_prov_gate_wire_register(), only allowlists exact paths), but a
// future consumer adding such a wildcard entry must account for reboot
// being reachable during provisioning as a result. The allowlist match
// itself is a pure UNION over its entries with no deny-entry type
// (bb_http_prov_gate.c -- any matching entry admits the path, order
// independent; see bb_http_prov_gate.h's own doc comment on why a narrower
// entry must never suppress a broader one), so once such a wildcard is added
// there is no way to carve reboot back out of it at the gate layer today --
// a deny/exclude mechanism is tracked separately, not part of this route move.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)

// Sized with headroom over the golden-proven composed body
// (test_bb_system_reboot_meta_golden.c's k_expected_meta_schema is 127
// bytes incl. NUL -- the hand literal's own sizeof is smaller since the
// composed body picks up ",\"required\":[],\"additionalProperties\":false}"
// the hand literal never had, see the accepted-delta doc comment above) --
// any future desync trips bb_serialize_meta_openapi_schema()'s own
// bounded-buffer BB_ERR_NO_SPACE contract instead of silently truncating.
static char s_reboot_request_schema_buf[192];

// Mutable (`.data`, not `.rodata`) with this config on -- `.request_schema`
// starts NULL and is patched in once by ensure_reboot_request_schema_patched()
// below (only on that composer's SUCCESS path), same NULL-then-patch shape
// as every `.request_schema`/`.schema` site in this codebase. Do NOT point
// this directly at s_reboot_request_schema_buf in the initializer: that
// link-time-constant address is always non-NULL even before the first
// compose, so a compose failure would leave bb_openapi_emit.c's
// `if (route->request_schema)` pointer-null check seeing a truthy-but-empty
// buffer and emitting a schema-less requestBody instead of omitting it
// entirely -- see bb_sensor_http_wire.c's PATCH /api/sensors/fan
// s_sensors_fan_patch_describe_route for the documented precedent (B1-1244,
// the failure mode this exact route used to have).
static bb_route_t s_reboot_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/diag/reboot",
    .tag                  = "system",
    .summary              = "Reboot the device",
    .request_content_type = "application/json",
    .request_schema       = NULL /* patched at init */,
    .responses            = s_reboot_responses,
    .handler              = reboot_handler,
};

#else

static const bb_route_t s_reboot_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/diag/reboot",
    .tag                  = "system",
    .summary              = "Reboot the device",
    .request_content_type = "application/json",
    .request_schema       = BB_SYSTEM_REBOOT_REQUEST_SCHEMA_LITERAL,
    .responses            = s_reboot_responses,
    .handler              = reboot_handler,
};

#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

// Compose-and-patch step, called from both bb_system_routes_init() and the
// test accessor below: idempotent (a second call is a pointer-stable no-op,
// guarded by bb_serialize_meta_ensure_composed()'s buf[0] sentinel) and
// fail-loud (propagates the composer's rc without ever leaving a
// partial schema in the buffer).
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
static bb_err_t ensure_reboot_request_schema_patched(void)
{
    bb_err_t rc = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                              &s_reboot_desc, &bb_system_reboot_meta,
                                              s_reboot_request_schema_buf,
                                              sizeof(s_reboot_request_schema_buf));
    if (rc != BB_OK) return rc;  // fail loud -- never patch a partial/NULL schema in
    s_reboot_route.request_schema = s_reboot_request_schema_buf;
    return BB_OK;
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

bb_err_t bb_system_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    // Documentation-only schema compose (config ON only): a compose failure
    // here must not abort real route registration below (the "reboot"
    // bb_data bind + POST /api/diag/reboot). s_reboot_route.request_schema starts
    // NULL and is patched to s_reboot_request_schema_buf only on
    // ensure_reboot_request_schema_patched()'s SUCCESS path (see that
    // route's own doc comment above), so on failure the field stays NULL --
    // and bb_openapi_emit.c's `if (route->request_schema)` gate omits
    // requestBody entirely rather than emitting an empty content block.
    // Degrade and continue: log a warning and fall through to registration.
    //
    // This init fn is portable and genuinely host-compiled (see
    // test_route_schema_literals_live.c and this component's own
    // bbtool-scaffold-hint), unlike the sibling ESP_PLATFORM-gated sites at
    // cddd5624 (bb_ota_hooks / bb_health_stack / bb_diag_http / bb_display) --
    // this branch IS host-testable; see test_bb_system_reboot_route_wiring.c.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    bb_err_t compose_rc = ensure_reboot_request_schema_patched();
    if (compose_rc != BB_OK) {
        bb_log_w(TAG, "reboot request schema compose failed (rc=%d), route ships without a schema", (int)compose_rc);
    }
#endif

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

// Runtime-compose test accessors (B1-1059 emit batch A, site 2) -- exercise
// the same guarded, idempotent compose-and-patch step bb_system_routes_init()
// runs, without requiring a real bb_http_handle_t server. Portable (not
// ESP_PLATFORM-gated), same posture as bb_diag_storage_nvs.c's pilot
// accessors.
bb_err_t bb_system_reboot_assemble_request_schema_for_test(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return ensure_reboot_request_schema_patched();
#else
    return BB_OK;
#endif
}

const char *bb_system_reboot_get_request_schema_for_test(void)
{
    // Reads the actual struct field the emitter dereferences
    // (bb_openapi_emit.c's `if (route->request_schema)`), not a buffer-
    // content proxy that can diverge from it -- s_reboot_route.
    // request_schema is NULL-then-patched (see that route's own doc
    // comment), same shape as every request/response schema in this file.
    return s_reboot_route.request_schema;
}
#endif /* BB_SYSTEM_TESTING */
