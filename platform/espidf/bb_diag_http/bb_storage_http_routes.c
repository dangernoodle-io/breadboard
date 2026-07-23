// bb_storage_http_routes — DELETE /api/diag/storage handler (B1-757).
//
// Portable: no ESP-IDF-specific includes. Compiled on both ESP-IDF and host
// (for unit tests). All storage access goes through the portable
// bb_storage_erase / bb_storage_erase_namespace facade — this file never
// calls a backend-specific (e.g. bb_storage_nvs_*) function directly, which
// is the whole point of rehoming this route off bb_nv.
//
// B1-1147/B1-859: request ingress migrated off bb_json onto bb_data_apply
// (the same template factory_reset_handler below, bb_wifi_http_routes.c's
// wifi_patch_handler, and bb_system_routes.c's reboot_handler already use).
// This was the LAST bb_json request-parser in the firmware.
//
// The one wrinkle this route has that its siblings don't: "namespace" is
// polymorphic (a string OR an array of strings). bb_serialize has no
// union/oneOf field type, but none is needed -- bb_serialize.h's populate
// contract explicitly SUPPORTS binding two fields to the SAME wire key (see
// its "Duplicate `.key`" doc, and bb_system_routes.c's s_reboot_fields[]
// live precedent). storage_delete_apply_t below binds "namespace" TWICE:
// once as BB_TYPE_STR (ns_str), once as BB_TYPE_ARR of BB_TYPE_STR (ns_arr).
// Each getter type-checks its own token (populate_get_str() requires
// bb_serialize_json_tok_is_str(); populate_begin_arr() requires
// bb_serialize_json_tok_is_arr()) and fails closed -- exactly one of the two
// ever succeeds for a given request, the other is left at its zero seed,
// indistinguishable from "absent". No new bb_type_t, no general oneOf/union
// mechanism.
//
// Fork (this PR): storage_delete_apply() -- the "storage_delete" binding's
// apply() hook -- does STRUCTURAL VALIDATION ONLY (confirm, the wipe_wifi
// guard, namespace presence/shape/cap, the key+array-namespace ambiguity,
// and the oversize-reject checks below) and never touches bb_storage
// itself. Unlike factory_reset_apply()/wifi_creds_apply() (which perform
// their destructive/staging action directly inside apply()),
// storage_delete_handler() below does the actual erase loop AFTER
// bb_data_apply() returns, reading the same dst_scratch it supplied --
// same "defer past return" posture bb_system_routes.c's reboot_handler
// uses, and for the same reason: this route's 200 response body is a
// per-request DYNAMIC list of successfully-erased namespace names, data
// apply()'s bb_err_t-only return has no channel to carry back out.
//
// Oversize-reject (not silent truncate): "backend"/"key"/"namespace" all
// select a destructive target, so a value that overflows its real limit is
// REJECTED (400) rather than truncated -- a truncated value could
// coincidentally collide with a different, real name and erase the wrong
// store. Reuses bb_wifi_http_routes.c's established idiom (see
// bb_wifi_pending_validate_buf()'s doc): give populate's get_str() MORE
// buffer than the real limit so an oversized value survives intact (or,
// past the buffer's own cap, is detectably "buffer-full") instead of being
// silently clipped down to exactly the real limit, then re-validate with
// strnlen(buf, cap) against the real limit before using the value for
// anything.
//
// One documented, intentional behavior change (review finding, this PR):
// `"namespace": ""` (explicit empty string) is now rejected 400 by
// storage_delete_apply()'s presence check, rather than reaching
// bb_storage_erase_namespace() -- see that check's own doc comment for the
// pre-migration outcome this replaces (a real-hardware ESP_ERR_NVS_INVALID_NAME
// misclassified as a generic 500, never a destructive wipe).
//
// storage_delete_too_long() below re-hand-rolls the same bounded
// strnlen(buf, cap) > max oversize-reject idiom bb_wifi_pending_validate_buf()
// (components/bb_wifi/bb_wifi_pending.c) already implements. Extracting a
// shared bb_str helper is a distinct, cross-component concern (bb_str +
// bb_wifi + bb_storage_http) tracked as its own follow-up, not done in this
// PR.

#include "bb_storage_http.h"
#include "../../../components/bb_diag_http/bb_storage_http_delete_wire_priv.h"
#include "bb_data.h"
#include "bb_http.h"
#include "bb_http_body.h"
#include "bb_http_server.h"
#include "bb_http_serialize_stream.h"
#include "bb_log.h"
#include "bb_serialize.h"
#include "bb_settings.h"
#include "bb_str.h"
#include "bb_system.h"

#ifdef ESP_PLATFORM
#include "bb_clock.h"
#include "bb_timer.h"
#include "esp_system.h"
#endif

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_storage_http";

#define BB_STORAGE_HTTP_DELETE_BODY_MAX        1024
#define BB_STORAGE_HTTP_BACKEND_MAX            16  // real cap INCLUDING the NUL byte -- max 15 usable chars
#define BB_STORAGE_HTTP_KEY_MAX                16  // same real-cap convention as BACKEND_MAX above
#define BB_STORAGE_HTTP_FACTORY_RESET_BODY_MAX 128

// Oversize-reject buffer slack (see the file header comment's "Oversize-
// reject" section) -- 16 spare bytes past each field's real cap, same slack
// bb_wifi_http_creds_wire_priv.h's BB_WIFI_HTTP_CREDS_WIRE_SSID_BUF/
// PASS_BUF use.
#define BB_STORAGE_HTTP_DELETE_BACKEND_BUF (BB_STORAGE_HTTP_BACKEND_MAX + 16)          // 32
#define BB_STORAGE_HTTP_DELETE_KEY_BUF     (BB_STORAGE_HTTP_KEY_MAX + 16)              // 32
// Namespace name real limit matches BB_STORAGE_HTTP_DELETE_NS_LEN (the
// wire's namespace-name bound, itself matching bb_storage_entry_t.
// ns_or_dir[16], the real NVS namespace-name limit) -- applied identically
// to the string-namespace form and to each array-namespace element.
#define BB_STORAGE_HTTP_DELETE_NAME_BUF    (BB_STORAGE_HTTP_DELETE_NS_LEN + 16)        // 32

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

static void send_412(bb_http_request_t *req, const char *msg)
{
    bb_http_resp_set_status(req, 412);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

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

static void send_501(bb_http_request_t *req, const char *msg)
{
    bb_http_resp_set_status(req, 501);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// "storage_delete" bb_data ingress binding -- backs DELETE /api/diag/storage.
// ---------------------------------------------------------------------------

// ns_arr's backing storage is sized BB_STORAGE_HTTP_DELETE_NS_MAX + 1, NOT
// BB_STORAGE_HTTP_DELETE_NS_MAX -- populate() reports the DESTINATION
// capacity it actually wrote (dst.count == min(source count, max_items)),
// never the source's true/unclamped count, so the only way to observe "the
// request named more than BB_STORAGE_HTTP_DELETE_NS_MAX namespaces" is to
// give populate ONE extra slot of capacity: any real count > MAX makes
// count come back as MAX+1 (clamped there), which storage_delete_apply()
// below rejects; a real count of exactly MAX comes back as MAX, accepted.
#define BB_STORAGE_HTTP_DELETE_NS_CAP (BB_STORAGE_HTTP_DELETE_NS_MAX + 1)

typedef struct {
    bool   confirm;
    bool   wipe_wifi;
    char   backend[BB_STORAGE_HTTP_DELETE_BACKEND_BUF];
    char   key[BB_STORAGE_HTTP_DELETE_KEY_BUF];
    // Dual-key "namespace" binding (see the file header comment) -- exactly
    // one of ns_str/ns_arr is ever populated for a given request; the other
    // stays at its storage_delete_gather() memset0 seed.
    char   ns_str[BB_STORAGE_HTTP_DELETE_NAME_BUF];
    char   ns_arr_names[BB_STORAGE_HTTP_DELETE_NS_CAP][BB_STORAGE_HTTP_DELETE_NAME_BUF];
    char  *ns_arr_items[BB_STORAGE_HTTP_DELETE_NS_CAP];
    bb_serialize_arr_t ns_arr;
} storage_delete_apply_t;

static const bb_serialize_field_t s_storage_delete_fields[] = {
    { .key = "confirm", .type = BB_TYPE_BOOL, .offset = offsetof(storage_delete_apply_t, confirm) },
    { .key = "wipe_wifi", .type = BB_TYPE_BOOL, .offset = offsetof(storage_delete_apply_t, wipe_wifi) },
    { .key = "backend", .type = BB_TYPE_STR, .offset = offsetof(storage_delete_apply_t, backend),
      .max_len = sizeof(((storage_delete_apply_t *)0)->backend) },
    { .key = "key", .type = BB_TYPE_STR, .offset = offsetof(storage_delete_apply_t, key),
      .max_len = sizeof(((storage_delete_apply_t *)0)->key) },
    // "namespace" bound TWICE -- once as a plain string, once as an array of
    // strings -- relying on bb_serialize.h's documented duplicate-`.key`
    // populate contract (see the file header comment above and
    // bb_system_routes.c's s_reboot_fields[] for the live precedent this
    // mirrors). Each getter independently type-checks its own wire token
    // and fails closed when it doesn't match, so field order here is
    // irrelevant and no cursor state is shared between the two reads. If
    // that contract is ever narrowed (a single forward-only cursor pass, or
    // duplicate-key rejection added to populate_check_fields()), this
    // binding breaks silently.
    { .key = "namespace", .type = BB_TYPE_STR, .offset = offsetof(storage_delete_apply_t, ns_str),
      .max_len = sizeof(((storage_delete_apply_t *)0)->ns_str) },
    { .key = "namespace", .type = BB_TYPE_ARR, .offset = offsetof(storage_delete_apply_t, ns_arr),
      .elem_type = BB_TYPE_STR, .max_len = BB_STORAGE_HTTP_DELETE_NAME_BUF,
      .max_items = BB_STORAGE_HTTP_DELETE_NS_CAP },
};

static const bb_serialize_desc_t s_storage_delete_desc = {
    .type_name = "storage_delete_apply_t",
    .fields    = s_storage_delete_fields,
    .n_fields  = 6,
    .snap_size = sizeof(storage_delete_apply_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1181a) -- co-located JSON Schema companion to
// s_storage_delete_desc above, gated behind BB_SERIALIZE_META_HOST (see
// bb_storage_http.h's banner). A genuine oneOf: "namespace" is bound TWICE
// (BB_TYPE_STR + BB_TYPE_ARR-of-STR, see s_storage_delete_fields' doc
// comment above), and BOTH occurrences are real, user-facing shapes -- the
// hand-authored s_storage_delete_route's request_schema below renders
// "namespace":{"oneOf":[{"type":"string"},{"type":"array","items":
// {"type":"string"}}]}. The meta row's two branches pair 1:1, in field
// order, with the two physical "namespace" occurrences (STR first, then
// ARR-of-STR); `.required` lives on the ONEOF row itself (mirrors
// request_schema's "required":["namespace","confirm"]). See
// test_bb_storage_http_delete_apply_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_HOST)

static const bb_serialize_field_meta_t s_storage_delete_ns_branches_rows[] = {
    { .key = "namespace" },  // branch 0: BB_TYPE_STR occurrence
    { .key = "namespace" },  // branch 1: BB_TYPE_ARR-of-STR occurrence
};

static const bb_serialize_field_meta_t *const s_storage_delete_ns_branches[] = {
    &s_storage_delete_ns_branches_rows[0],
    &s_storage_delete_ns_branches_rows[1],
    NULL,
};

static const bb_serialize_field_meta_t s_storage_delete_meta_rows[] = {
    { .key = "confirm", .required = true },
    { .key = "wipe_wifi" },
    { .key = "backend" },
    { .key = "key" },
    { .key = "namespace", .required = true, .kind = BB_SERIALIZE_META_KIND_ONEOF,
      .branches = s_storage_delete_ns_branches, .n_branches = 2 },
};

const bb_serialize_desc_meta_t bb_storage_http_delete_apply_meta = {
    .type_name = "storage_delete_apply_t",
    .rows      = s_storage_delete_meta_rows,
    .n_rows    = sizeof(s_storage_delete_meta_rows) / sizeof(s_storage_delete_meta_rows[0]),
};

#ifdef BB_STORAGE_HTTP_TESTING
const bb_serialize_desc_t *bb_storage_http_delete_apply_desc_for_test(void)
{
    return &s_storage_delete_desc;
}
#endif /* BB_STORAGE_HTTP_TESTING */

#endif /* BB_SERIALIZE_META_HOST */

// Seed hook -- NOT a stub, and NOT egress-only despite the "gather" name
// (unlike every other binding in this file/its siblings): this route drives
// bb_data_apply() under BB_DATA_APPLY_PATCH specifically so THIS hook runs
// (bb_data_commit() calls gather() BEFORE bb_serialize_populate(), whereas
// BB_DATA_APPLY_POST's own zero-fill is entirely internal to bb_data with
// no caller hook point) -- the one place in the call chain where dst_scratch
// is both zeroed AND still directly reachable by this file's own code,
// which is exactly what ns_arr's array contract requires: "the destination
// bb_serialize_arr_t must already have `.items` pre-wired by the caller to
// writable, contiguous storage" (bb_serialize.h) BEFORE populate runs. A
// plain BB_DATA_APPLY_POST binding cannot satisfy that for a BB_TYPE_ARR
// field at all -- bb_data_commit()'s POST branch does its own internal
// `memset(dst_scratch, 0, snap_size)` with no hook in between, which would
// wipe out any pre-wiring this file did on dst_scratch before the
// bb_data_apply() call returns control to populate.
//
// This hook is otherwise a full replace (a bare memset, same as
// BB_DATA_APPLY_POST would do), never a real gather-from-existing-state --
// so it stays exactly as "side-effect-free and infallible" as
// bb_data_gather_fn's PATCH-seed contract requires, it just also happens to
// run the array pre-wiring bb_serialize_populate() needs.
static bb_err_t storage_delete_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    storage_delete_apply_t *s = (storage_delete_apply_t *)dst;
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < BB_STORAGE_HTTP_DELETE_NS_CAP; i++) {
        s->ns_arr_items[i] = s->ns_arr_names[i];
    }
    s->ns_arr.items = s->ns_arr_items;
    s->ns_arr.count = 0;
    return BB_OK;
}

// Bounded-buffer length check for the oversize-reject idiom (see the file
// header comment) -- `buf` is a caller-zeroed, oversized buffer of capacity
// `buf_cap`; returns true if the value stored in it (bounded by strnlen, so
// a buffer-full, non-NUL-terminated value is treated as "at least buf_cap
// long", never read past its own end) exceeds `real_max`.
static bool storage_delete_too_long(const char *buf, size_t buf_cap, size_t real_max)
{
    return strnlen(buf, buf_cap) > real_max;
}

// Ingress hook: STRUCTURAL VALIDATION ONLY -- see the file header comment
// for why the actual erase + response building stay in
// storage_delete_handler(), after bb_data_apply() returns.
//
// Status-code mapping (storage_delete_handler() below):
//   BB_ERR_CONFLICT   -> 412 (confirm / wipe_wifi guard)
//   BB_ERR_VALIDATION -> 400 (everything else: absent/wrong-type/oversized
//                             namespace, array-over-cap, key+array combo,
//                             oversized backend/key)
static bb_err_t storage_delete_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    const storage_delete_apply_t *s = snap;

    if (!s->confirm) {
        return BB_ERR_CONFLICT;
    }

    if (storage_delete_too_long(s->backend, sizeof(s->backend), BB_STORAGE_HTTP_BACKEND_MAX - 1)) {
        return BB_ERR_VALIDATION;
    }
    if (storage_delete_too_long(s->key, sizeof(s->key), BB_STORAGE_HTTP_KEY_MAX - 1)) {
        return BB_ERR_VALIDATION;
    }

    bool ns_is_str = s->ns_str[0] != '\0';
    bool ns_is_arr = s->ns_arr.count > 0;

    // Absent, or present as neither a (non-empty) string nor a (non-empty)
    // array -- both a genuinely missing "namespace" and one present with
    // the wrong wire type (a number, bool, object, ...) leave BOTH ns_str
    // and ns_arr at their zero seed, so they are indistinguishable here and
    // share one 400.
    //
    // This ALSO covers `"namespace": ""` (an explicit empty string):
    // ns_str[0] == '\0' is identical to the zero seed, so it is rejected
    // here too, BEFORE reaching bb_storage_erase_namespace() -- a
    // deliberate, DOCUMENTED behavior change from the pre-migration
    // handler (B1-1147 review finding). Pre-migration, an empty-string
    // "namespace" passed bb_json's is-string check and reached
    // bb_storage_erase_namespace(backend, "") unbounded; on the real
    // bb_storage_nvs backend that call's own nvs_open("", ...) returns
    // ESP-IDF's ESP_ERR_NVS_INVALID_NAME (a namespace name "shouldn't be
    // empty" per nvs_open()'s own contract) -- NOT BB_ERR_UNSUPPORTED, so
    // the old handler's overall-error mapping fell through to its generic
    // 500 branch. It was never a destructive whole-namespace/whole-backend
    // wipe; it was already a rejected request, just misclassified as a
    // 500 server error instead of a 400 client error. Rejecting it here
    // instead turns that into a correct 400, fail-closed, before ever
    // reaching the backend -- a status-code fix, not a regression.
    if (!ns_is_str && !ns_is_arr) {
        return BB_ERR_VALIDATION;
    }

    if (ns_is_str && storage_delete_too_long(s->ns_str, sizeof(s->ns_str), BB_STORAGE_HTTP_DELETE_NS_LEN - 1)) {
        return BB_ERR_VALIDATION;
    }

    if (ns_is_arr) {
        // Fail closed BEFORE any erase is performed -- see
        // BB_STORAGE_HTTP_DELETE_NS_CAP's doc comment above for how a
        // real count > BB_STORAGE_HTTP_DELETE_NS_MAX is detected here.
        if (s->ns_arr.count > BB_STORAGE_HTTP_DELETE_NS_MAX) {
            return BB_ERR_VALIDATION;
        }
        for (size_t i = 0; i < s->ns_arr.count; i++) {
            if (storage_delete_too_long(s->ns_arr_names[i], sizeof(s->ns_arr_names[i]),
                                         BB_STORAGE_HTTP_DELETE_NS_LEN - 1)) {
                return BB_ERR_VALIDATION;
            }
        }
    }

    bool has_key = s->key[0] != '\0';
    if (has_key && ns_is_arr) {
        return BB_ERR_VALIDATION;
    }

    // wipe_wifi guard: ASK bb_settings which namespace holds wifi creds
    // rather than comparing against a copied "bb_cfg" literal -- a copy
    // would silently go stale if bb_settings' namespace ever changed.
    bool needs_wipe_wifi = false;
    if (ns_is_str) {
        needs_wipe_wifi = bb_settings_ns_is_wifi_creds(s->ns_str);
    } else {
        for (size_t i = 0; i < s->ns_arr.count; i++) {
            if (bb_settings_ns_is_wifi_creds(s->ns_arr_names[i])) {
                needs_wipe_wifi = true;
                break;
            }
        }
    }
    if (needs_wipe_wifi && !s->wipe_wifi) {
        return BB_ERR_CONFLICT;
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — JSON body
// ---------------------------------------------------------------------------

// JSON parse scratch: worst case (an array namespace at
// BB_STORAGE_HTTP_DELETE_NS_MAX elements) is a root object + 5 scalar/
// string fields + one array token + up to 16 element tokens == ~23 tokens,
// comfortably under the token recorder's own default-capacity pool (48 *
// sizeof(bb_serialize_json_tok_t) == 2304 bytes) -- this must clear that
// plus the control structs plus headroom for the escape-decode arena
// regardless of body size, same fixed-pool-size rationale as
// bb_wifi_http_routes.c's WIFI_PATCH_PARSE_SCRATCH_BYTES /
// bb_system_routes.c's BB_SYSTEM_REBOOT_PARSE_SCRATCH_BYTES /
// FACTORY_RESET_PARSE_SCRATCH_BYTES below.
#define STORAGE_DELETE_PARSE_SCRATCH_BYTES 3072

static bb_err_t storage_delete_handler(bb_http_request_t *req)
{
    // BB_STORAGE_HTTP_DELETE_BODY_MAX is MAX BODY BYTES (see
    // bb_http_req_recv_body_stack()'s cap-semantics doc); the stack buffer
    // itself is sized one byte larger for the NUL terminator.
    char   body[BB_STORAGE_HTTP_DELETE_BODY_MAX + 1];
    size_t n = 0;
    if (bb_http_req_recv_body_stack(req, body, sizeof(body), &n) != BB_OK) {
        send_400(req, "missing, oversized, or unreadable body");
        return BB_ERR_INVALID_ARG;
    }

    // dst_scratch is intentionally left uninitialized here -- storage_
    // delete_gather() (invoked via BB_DATA_APPLY_PATCH below, BEFORE
    // bb_serialize_populate() runs) fully zeroes it and pre-wires ns_arr's
    // storage; see that hook's own doc comment for why PATCH mode (not
    // POST) is required to make a BB_TYPE_ARR field work through
    // bb_data_apply() at all.
    storage_delete_apply_t dst_scratch;

    char parse_scratch[STORAGE_DELETE_PARSE_SCRATCH_BYTES];
    bb_data_apply_req_t apply_req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "storage_delete",
        .mode              = BB_DATA_APPLY_PATCH,
        .body              = body,
        .body_len          = n,
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = &dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };
    bb_err_t rc = bb_data_apply(&apply_req);

    // Same disjoint parse-layer codes (bb_core.h, B1-1090) every other
    // bb_data-fed route in this file/component maps to 400 -- a body that
    // was actually sent but fails to parse (grammar-invalid or truncated).
    if (rc == BB_ERR_PARSE_GRAMMAR || rc == BB_ERR_PARSE_INCOMPLETE) {
        send_400(req, "invalid JSON");
        return rc;
    }
    // storage_delete_apply()'s confirm/wipe_wifi guard -- see its own doc
    // comment for the full mapping.
    if (rc == BB_ERR_CONFLICT) {
        send_412(req,
            "pass \"confirm\": true to confirm storage deletion; if the "
            "namespace contains wifi credentials, also pass "
            "\"wipe_wifi\": true. Note: if CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP "
            "is enabled, credentials are restored from RTC on next boot.");
        return rc;
    }
    // storage_delete_apply()'s structural rejects (namespace absent/wrong
    // type/oversized/over-cap, key+array-namespace ambiguity, oversized
    // backend/key).
    if (rc == BB_ERR_VALIDATION) {
        send_400(req,
            "invalid request: \"namespace\" (required, string or array of "
            "strings, each entry within the real namespace-name limit), "
            "\"backend\"/\"key\" length, or \"key\" with an array namespace");
        return rc;
    }
    if (rc != BB_OK) {
        send_500(req, "storage delete request processing failed");
        return rc;
    }

    // Every check above passed -- resolve the defaults/discriminant
    // dst_scratch itself doesn't carry (backend's "nvs" default; which of
    // ns_str/ns_arr is populated) and perform the actual erase(s). This
    // stays here (not in storage_delete_apply()) because the 200 response
    // needs the per-request list of namespaces actually erased, which
    // apply()'s bb_err_t-only return has no channel to carry back out --
    // same "defer past bb_data_apply() return" posture bb_system_routes.c's
    // reboot_handler uses, and for the same reason (per-request output data
    // apply() cannot surface).
    const char *backend  = dst_scratch.backend[0] ? dst_scratch.backend : "nvs";
    bool        ns_is_arr = dst_scratch.ns_arr.count > 0;
    bool        has_key   = dst_scratch.key[0] != '\0';

    bb_err_t overall = BB_OK;
    char     deleted_names[BB_STORAGE_HTTP_DELETE_NS_MAX][BB_STORAGE_HTTP_DELETE_NS_LEN];
    size_t   n_deleted = 0;

    if (!ns_is_arr) {
        const char *ns = dst_scratch.ns_str;
        bb_err_t    err;
        if (has_key) {
            bb_storage_addr_t addr = { .backend = backend, .ns_or_dir = ns, .key = dst_scratch.key };
            err = bb_storage_erase(&addr);
            bb_log_i(TAG, "DELETE /api/diag/storage backend=%s ns=%s key=%s -> %d",
                     backend, ns, dst_scratch.key, (int)err);
        } else {
            err = bb_storage_erase_namespace(backend, ns);
            bb_log_i(TAG, "DELETE /api/diag/storage backend=%s ns=%s (all) -> %d",
                     backend, ns, (int)err);
        }
        if (err != BB_OK) {
            overall = err;
        } else {
            bb_strlcpy(deleted_names[n_deleted], ns, sizeof(deleted_names[n_deleted]));
            n_deleted++;
        }
    } else {
        // array of namespaces — erase each in turn; continue on individual errors
        for (size_t i = 0; i < dst_scratch.ns_arr.count; i++) {
            const char *ns = dst_scratch.ns_arr_names[i];
            if (ns[0] == '\0') continue;
            bb_err_t err = bb_storage_erase_namespace(backend, ns);
            bb_log_i(TAG, "DELETE /api/diag/storage backend=%s ns=%s (all) -> %d",
                     backend, ns, (int)err);
            if (err != BB_OK) {
                if (overall == BB_OK) overall = err;
            } else {
                bb_strlcpy(deleted_names[n_deleted], ns, sizeof(deleted_names[n_deleted]));
                n_deleted++;
            }
        }
    }

    if (overall != BB_OK) {
        if (overall == BB_ERR_UNSUPPORTED) {
            send_501(req, "backend does not support namespace-level erase");
        } else {
            send_500(req, "one or more storage erase operations failed");
        }
        return overall;
    }

    /* 200 {"deleted": [...], "key": <optional>} via bb_serialize -- "key" is
     * omitted entirely when !has_key (see bb_storage_http_delete_wire_desc's
     * present-predicate). */
    bb_storage_http_delete_wire_t snap;
    bb_storage_http_delete_wire_fill(&snap, deleted_names, n_deleted, dst_scratch.key, has_key);

    return bb_http_serialize_stream(req, &bb_storage_http_delete_wire_desc, &snap);
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const bb_route_response_t s_storage_delete_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"deleted\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"key\":{\"type\":\"string\"}},"
      "\"required\":[\"deleted\"]}",
      "deletion successful; 'key' present only when a single key was erased" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "missing/invalid namespace, or key+array-namespace combo" },
    { 412, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "missing confirm:true / missing wipe_wifi:true for the wifi-creds namespace" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "storage erase operation failed" },
    { 501, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "backend does not support namespace-level erase" },
    { 0 },
};

static const bb_route_t s_storage_delete_route = {
    .method           = BB_HTTP_DELETE,
    .path             = "/api/diag/storage",
    .tag              = "diag",
    .summary          = "Delete one storage key or one/multiple namespaces via JSON body. "
                        "Requires {\"confirm\":true}. Works against any registered bb_storage "
                        "backend (\"backend\", default \"nvs\"). For the wifi-creds namespace "
                        "(bb_settings-owned) also requires {\"wipe_wifi\":true} (contains wifi "
                        "credentials; RTC backup may restore them on next boot if "
                        "CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP is enabled). Use an array namespace to "
                        "reset multiple namespaces in one call (e.g. [\"bb_mqtt\",\"bb_udp\",\"bb_tcp\"]). "
                        "\"key\" is forbidden when namespace is an array.",
    .request_schema   = "{\"type\":\"object\","
                        "\"properties\":{"
                        "\"backend\":{\"type\":\"string\"},"
                        "\"namespace\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"array\",\"items\":{\"type\":\"string\"}}]},"
                        "\"key\":{\"type\":\"string\"},"
                        "\"confirm\":{\"type\":\"boolean\"},"
                        "\"wipe_wifi\":{\"type\":\"boolean\"}},"
                        "\"required\":[\"namespace\",\"confirm\"]}",
    .request_content_type = "application/json",
    .responses        = s_storage_delete_responses,
    .parameters       = NULL,
    .parameters_count = 0,
    .handler          = storage_delete_handler,
};

// ---------------------------------------------------------------------------
// POST /api/diag/factory-reset — whole-"nvs"-backend erase + reboot
// (B1-960, rehomed off bb_nv's POST /api/factory-reset /
// bb_nv_factory_reset_routes_init). Request schema PRESERVED exactly from
// the old bb_nv route: {"confirm":"factory-reset"} exact-string match.
//
// Gated behind CONFIG_BB_STORAGE_HTTP_FACTORY_RESET (default n — see
// bb_storage_http.h). Mirrors the deleted bb_nv_routes.c's
// `#if CONFIG_BB_NV_FACTORY_RESET` posture: destructive routes are opt-in.
//
// B1-859: ingress migrated off bb_json onto bb_data_apply (the B1-1022
// template) -- a single "factory_reset" bb_data binding backs this route.
// This site FITS the template with NO fork: BB_DATA_APPLY_POST's memset0
// seed leaves an absent "confirm" as an empty string, which the exact-match
// strcmp() below already correctly rejects -- no sentinel/oversize-reject
// trick needed (unlike the wifi cutover's ssid/pass fields), since a
// truncated confirm value can never accidentally equal "factory-reset".
// ---------------------------------------------------------------------------

#if defined(CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) && CONFIG_BB_STORAGE_HTTP_FACTORY_RESET

typedef struct {
    char confirm[32];
} bb_storage_factory_reset_apply_t;

static const bb_serialize_field_t s_factory_reset_fields[] = {
    { .key = "confirm", .type = BB_TYPE_STR, .offset = offsetof(bb_storage_factory_reset_apply_t, confirm),
      .max_len = sizeof(((bb_storage_factory_reset_apply_t *)0)->confirm) },
};

static const bb_serialize_desc_t s_factory_reset_desc = {
    .type_name = "bb_storage_factory_reset_apply_t",
    .fields    = s_factory_reset_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bb_storage_factory_reset_apply_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-1) -- co-located JSON Schema
// companion to s_factory_reset_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_storage_http.h's banner). "required"
// mirrors the "required" array of s_factory_reset_route's hand-authored
// request_schema literal below (["confirm"]). See
// test_bb_storage_http_factory_reset_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_HOST)

static const bb_serialize_field_meta_t s_factory_reset_meta_rows[] = {
    { .key = "confirm", .required = true },
};

const bb_serialize_desc_meta_t bb_storage_http_factory_reset_meta = {
    .type_name = "bb_storage_factory_reset_apply_t",
    .rows      = s_factory_reset_meta_rows,
    .n_rows    = sizeof(s_factory_reset_meta_rows) / sizeof(s_factory_reset_meta_rows[0]),
};

#ifdef BB_STORAGE_HTTP_TESTING
const bb_serialize_desc_t *bb_storage_http_factory_reset_desc_for_test(void)
{
    return &s_factory_reset_desc;
}
#endif /* BB_STORAGE_HTTP_TESTING */

#endif /* BB_SERIALIZE_META_HOST */

// Egress hook: exists only to satisfy bb_data_bind()'s non-NULL-gather
// invariant (a binding with no gather is rejected outright) -- this route is
// POST-only, so gather is never actually invoked (BB_DATA_APPLY_POST always
// memset0-seeds dst_scratch, see bb_data_apply()'s doc). Same posture as
// wifi_creds_gather() in bb_wifi_http_routes.c.
static bb_err_t factory_reset_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    memset(dst, 0, sizeof(bb_storage_factory_reset_apply_t));
    return BB_OK;
}

// Ingress hook: the exact-match confirm check plus the actual destructive
// work (whole-"nvs"-backend erase + RTC creds mirror invalidation) --
// response shaping (202 body) and the deferred reboot stay in the route
// below, keeping this fn HTTP-agnostic (bb_data_apply_fn's contract: return
// BB_ERR_VALIDATION for a domain-level reject so an HTTP-agnostic caller's
// err->status mapping stays a simple switch).
static bb_err_t factory_reset_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    const bb_storage_factory_reset_apply_t *creds = (const bb_storage_factory_reset_apply_t *)snap;

    if (strcmp(creds->confirm, "factory-reset") != 0) {
        return BB_ERR_VALIDATION;
    }

    bb_err_t err = bb_storage_erase_all("nvs");
    if (err != BB_OK) {
        return err;
    }

    /* Invalidate the shared RTC mirror so the restore-heal path does NOT
     * re-populate credentials on next boot. Best-effort/fail-open, same
     * posture as every other mirror clear in this codebase: an
     * unregistered "rtc" backend (RTC-backup Kconfig-disabled builds, or a
     * composer that never registered bb_storage_rtc) is harmless here.
     *
     * On device this stays gated on CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP (the
     * mirror only exists when that feature is on). On host there is no
     * sdkconfig.h / CONFIG_ bridge at all, so the call is unconditional —
     * same posture as the deleted bb_nv_config_factory_reset() host stub,
     * which called this unconditionally and was covered by
     * test_nv_factory_reset_invalidates_rtc_mirror. Keeping it unconditional
     * on host (rather than compiling it out) is what makes this
     * host-testable (B1-935 stale-creds-survive-factory-reset class). */
#if !defined(ESP_PLATFORM) || defined(CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP)
    bb_settings_wifi_rtc_mirror_clear();
#endif

    return BB_OK;
}

#ifdef ESP_PLATFORM
static void factory_reset_reboot_work_fn(void *arg)
{
    (void)arg;
    uint32_t uptime_s = (uint32_t)(bb_clock_now_ms64() / 1000ULL);
    /* epoch_s=0: no bb_ntp dependency here (would create an unwanted edge);
     * the boot-side reader treats epoch_s=0 as "unknown/unsynced" per the
     * record contract. */
    bb_err_t rc = bb_system_reboot_record_save(BB_RESET_SRC_FACTORY_RESET, NULL, 0, uptime_s);
    if (rc == BB_ERR_INVALID_ARG) {
        bb_log_w(TAG, "factory_reset: record encode failed, rebooting without reason");
    } else if (rc != BB_OK) {
        bb_log_w(TAG, "factory_reset: NVS persist failed: %d", (int)rc);
    }
    esp_restart();
}
#endif /* ESP_PLATFORM */

// JSON parse scratch: a flat 1-string-field document, comfortably under the
// route's own 128-byte body cap -- but the token recorder's own
// default-capacity pool alone is 48 * sizeof(bb_serialize_json_tok_t) ==
// 2304 bytes, so this must clear that plus the control structs plus
// headroom for the escape-decode arena regardless of body size (see
// bb_wifi_http_routes.c's WIFI_PATCH_PARSE_SCRATCH_BYTES, the same
// fixed-pool-size rationale).
#define FACTORY_RESET_PARSE_SCRATCH_BYTES 3072

static bb_err_t factory_reset_handler(bb_http_request_t *req)
{
    // BB_STORAGE_HTTP_FACTORY_RESET_BODY_MAX is MAX BODY BYTES (see
    // bb_http_req_recv_body_stack()'s cap-semantics doc); the stack buffer
    // itself is sized one byte larger for the NUL terminator.
    char   body[BB_STORAGE_HTTP_FACTORY_RESET_BODY_MAX + 1];
    size_t n = 0;
    if (bb_http_req_recv_body_stack(req, body, sizeof(body), &n) != BB_OK) {
        send_400(req, "missing, oversized, or unreadable body");
        return BB_ERR_INVALID_ARG;
    }

    bb_storage_factory_reset_apply_t dst_scratch;
    char                             parse_scratch[FACTORY_RESET_PARSE_SCRATCH_BYTES];
    bb_data_apply_req_t apply_req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "factory_reset",
        .mode              = BB_DATA_APPLY_POST,
        .body              = body,
        .body_len          = n,
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = &dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };
    bb_err_t rc = bb_data_apply(&apply_req);

    // Response shaping stays here in the route -- bb_data_apply()/apply()
    // stay HTTP-agnostic and never see a status code, only a bb_err_t (same
    // Fork-3 posture as the wifi PATCH cutover). BB_ERR_VALIDATION covers a
    // wrong/missing confirm value -- "the request body parsed fine but its
    // content is bad", 400. BB_ERR_PARSE_GRAMMAR/BB_ERR_PARSE_INCOMPLETE
    // cover a body the JSON backend's parse fn couldn't scan to completion
    // (grammar-invalid, e.g. "not-json", or truncated mid-object) -- same
    // "the request body itself is bad" bucket, also 400 (parity with the
    // pre-B1-859 bb_json handler, which returned 400 for any unparseable
    // body regardless of cause). These are the disjoint parse-layer codes
    // from bb_core.h (B1-1090's fix): they used to alias BB_ERR_INVALID_STATE,
    // which factory_reset_apply()'s own bb_storage_erase_all() call can
    // ALSO genuinely return from the flash/wear-levelling layer on a real
    // failed erase -- a bare BB_ERR_INVALID_STATE is therefore NOT mapped
    // to 400 here and instead falls through to the generic 500 branch
    // below, same as any other apply()-stage failure.
    // BB_ERR_UNSUPPORTED covers the backend-does-not-support-whole-backend-
    // erase case (factory_reset_apply() propagates bb_storage_erase_all()'s
    // own BB_ERR_UNSUPPORTED unchanged), 501 -- unlike bb_wifi_http's mapping,
    // where the same code means "format/binding not wired" and is client-ish
    // (400); here it means the storage backend genuinely lacks erase_all, a
    // real 501. There is no BB_ERR_INVALID_ARG row (bb_wifi_http lists one
    // defensively): it isn't reachable from client input on this route --
    // s_factory_reset_desc is a single BB_TYPE_STR field, and the only
    // INVALID_ARG source in bb_serialize_populate() is a static descriptor
    // property (max_items == 0 on an ARR field), which doesn't apply here.
    // Everything else (including a genuine BB_ERR_INVALID_STATE erase failure,
    // or the composition-invariant case of this fn's own "factory_reset"
    // binding somehow being unbound/unparsable -- unreachable given correct
    // wiring below) is a 500.
    if (rc == BB_ERR_VALIDATION) {
        send_400(req, "confirm field must be \"factory-reset\"");
        return rc;
    }
    if (rc == BB_ERR_PARSE_GRAMMAR || rc == BB_ERR_PARSE_INCOMPLETE) {
        send_400(req, "invalid JSON");
        return rc;
    }
    if (rc == BB_ERR_UNSUPPORTED) {
        send_501(req, "backend does not support whole-backend erase");
        return rc;
    }
    if (rc != BB_OK) {
        send_500(req, "factory reset failed");
        return rc;
    }

    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "factory_reset_accepted");
    bb_http_resp_json_obj_set_bool(&obj, "reboot", true);
    bb_http_resp_json_obj_end(&obj);

#ifdef ESP_PLATFORM
    static bb_oneshot_timer_t s_reset_timer = NULL;
    if (!s_reset_timer) {
        bb_timer_deferred_oneshot_create(factory_reset_reboot_work_fn, NULL,
                                         "bb_storage_http_factory_reset", &s_reset_timer);
    }
    bb_timer_oneshot_stop(s_reset_timer);
    bb_timer_oneshot_start(s_reset_timer, 500 * 1000); /* 500 ms — lets HTTP 202 flush */
#endif /* ESP_PLATFORM */

    return BB_OK;
}

static const bb_route_response_t s_factory_reset_responses[] = {
    { 202, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"status\":{\"type\":\"string\"},"
      "\"reboot\":{\"type\":\"boolean\"}},"
      "\"required\":[\"status\",\"reboot\"]}",
      "factory reset accepted; device will reboot after ~500 ms" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "missing or invalid confirmation body" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "factory reset (nvs erase) failed" },
    { 501, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "backend does not support whole-backend erase" },
    { 0 },
};

static const bb_route_t s_factory_reset_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/diag/factory-reset",
    .tag                  = "diag",
    .summary              = "Erase the whole \"nvs\" bb_storage backend and reboot to factory defaults",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\","
                            "\"properties\":{\"confirm\":{\"type\":\"string\"}},"
                            "\"required\":[\"confirm\"]}",
    .responses            = s_factory_reset_responses,
    .handler              = factory_reset_handler,
};

bb_err_t bb_storage_http_factory_reset_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_data_binding_t factory_reset_binding = {
        .key    = "factory_reset",
        .desc   = &s_factory_reset_desc,
        .gather = factory_reset_gather,
        .apply  = factory_reset_apply,
    };
    bb_err_t err = bb_data_bind(&factory_reset_binding);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_factory_reset_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "factory reset route registered");
    return BB_OK;
}

#ifdef BB_STORAGE_HTTP_TESTING
bb_err_t bb_storage_http_factory_reset_handler_for_test(bb_http_request_t *req)
{
    return factory_reset_handler(req);
}

// Test-only: binds the "factory_reset" bb_data key against the production
// gather/apply hooks without going through bb_storage_http_factory_reset_routes_init()
// (which additionally requires a real bb_http_handle_t server -- unavailable
// in host tests). Call after bb_data_test_reset() and before driving
// bb_storage_http_factory_reset_handler_for_test().
bb_err_t bb_storage_http_factory_reset_bind_for_test(void)
{
    bb_data_binding_t factory_reset_binding = {
        .key    = "factory_reset",
        .desc   = &s_factory_reset_desc,
        .gather = factory_reset_gather,
        .apply  = factory_reset_apply,
    };
    return bb_data_bind(&factory_reset_binding);
}
#endif /* BB_STORAGE_HTTP_TESTING */

#endif /* defined(CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) && CONFIG_BB_STORAGE_HTTP_FACTORY_RESET */

// ---------------------------------------------------------------------------
// Init: register the single (always-on) DELETE route
// ---------------------------------------------------------------------------

bb_err_t bb_storage_http_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_data_binding_t storage_delete_binding = {
        .key    = "storage_delete",
        .desc   = &s_storage_delete_desc,
        .gather = storage_delete_gather,
        .apply  = storage_delete_apply,
    };
    bb_err_t err = bb_data_bind(&storage_delete_binding);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_storage_delete_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "storage delete route registered");
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Testing exposure (host unit tests) — non-factory-reset handler
// ---------------------------------------------------------------------------

#ifdef BB_STORAGE_HTTP_TESTING
bb_err_t bb_storage_http_delete_handler_for_test(bb_http_request_t *req)
{
    return storage_delete_handler(req);
}

// Test-only: binds the "storage_delete" bb_data key against the production
// gather/apply hooks without going through bb_storage_http_routes_init()
// (which additionally requires a real bb_http_handle_t server -- unavailable
// in host tests). Call after bb_data_test_reset() and before driving
// bb_storage_http_delete_handler_for_test().
bb_err_t bb_storage_http_delete_bind_for_test(void)
{
    bb_data_binding_t storage_delete_binding = {
        .key    = "storage_delete",
        .desc   = &s_storage_delete_desc,
        .gather = storage_delete_gather,
        .apply  = storage_delete_apply,
    };
    return bb_data_bind(&storage_delete_binding);
}
#endif /* BB_STORAGE_HTTP_TESTING */
