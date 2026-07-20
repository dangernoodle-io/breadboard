#pragma once

/**
 * @brief Shared method+prefix HTTP section dispatch helper
 * (bb_http_section PR). CONSOLIDATES the URI-strip -> section-lookup ->
 * method-branch -> err-to-status pattern bb_diag_section.c already
 * hand-rolls (components/bb_diag/bb_diag_section.c) -- a second consumer
 * hand-rolling the same shape triggers extraction (workspace reuse-or-extract
 * convention). Mirrors bb_diag_section's own precedent: portable
 * registry/dispatch logic here, an ESP-IDF-only glue TU pulling values out of
 * bb_http_request_t in platform/espidf/bb_http_server/
 * bb_http_section_dispatch.c.
 *
 * REGISTRY-AGNOSTIC BY DESIGN: a namespace's render/apply hooks are plain
 * function pointers, never bb_data types -- bb_http_server stays free of any
 * bb_data dependency (a bb_data-specific router was considered and rejected:
 * it would only ever serve bb_data consumers and bakes a layering inversion
 * into bb_http_server). A consumer registers a namespace (prefix ->
 * render/apply) via bb_http_section_register_ns(), typically from a
 * composition root; the ESP-IDF dispatcher (bb_http_section_init()) then
 * serves both GET and PATCH for every registered namespace's prefix.
 *
 * THIS PR SHIPS THE MECHANISM ONLY -- zero real namespaces (mirrors
 * bb_diag_section's own "MECHANISM ONLY" precedent). Producers (bb_sensors:
 * fan/power/thermal) land in a later PR against this same API.
 */

#include "bb_core.h"
#include "bb_serialize.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed table capacity -- a handful of composed namespaces, not an
// open-ended runtime set (mirrors bb_diag_section's own small-table sizing
// rationale). No Kconfig bridge -- this is a compile-time-only constant.
#define BB_HTTP_SECTION_TABLE_CAP 16

// Max length (including the terminating NUL) of a namespace's prefix (e.g.
// "/api/sensors/"). bb_http_section_register_ns() rejects (BB_ERR_INVALID_ARG)
// any prefix whose strlen() is >= this bound rather than silently truncating
// it.
#define BB_HTTP_SECTION_PREFIX_MAX 32

// Max length (including the terminating NUL) of the "<name>" segment
// stripped from a request URI after its matched namespace's prefix.
#define BB_HTTP_SECTION_NAME_MAX 32

// GET hook: renders `name`'s current value into `buf` (capacity `cap`),
// writing the rendered length into `*out_len`. `query` is the request's
// query params (NULL for a query-less GET). `ctx` is the namespace's own
// bind-time context (bb_http_section_ns_t.ctx). NULL on a
// bb_http_section_ns_t means GET is unsupported for this namespace (the
// dispatcher returns 405 without calling anything).
typedef bb_err_t (*bb_http_section_render_fn)(const char *name,
                                               const bb_serialize_query_t *query,
                                               char *buf, size_t cap, size_t *out_len,
                                               void *ctx);

// Which pipeline stage produced an apply()'s bb_err_t. A flat bb_err_t can't
// tell "the wire body itself was bad" (400-class) apart from "the body
// decoded fine but a downstream step rejected/failed it" (400/500-class,
// depending on the step) -- see bb_data_parse()/bb_data_commit()
// (components/bb_data) for the split this mirrors. A namespace's apply()
// hook reports which stage its own bb_err_t belongs to so the dispatcher's
// status mapping (bb_http_section_status_for_apply(), src/
// bb_http_section_status.c) is correct BY CONSTRUCTION rather than guessed
// from the error code alone.
typedef enum {
    BB_HTTP_SECTION_STAGE_PARSE  = 0,  // the wire body itself couldn't be decoded/resolved
    BB_HTTP_SECTION_STAGE_COMMIT = 1,  // decode succeeded; a downstream step rejected or failed
} bb_http_section_stage_t;

typedef struct {
    bb_http_section_stage_t stage;
    bb_err_t                rc;
} bb_http_section_apply_result_t;

// PATCH/POST hook: applies `body` (`body_len` bytes) to `name`'s bound
// destination, returning a stage-tagged result (see
// bb_http_section_apply_result_t above). `ctx` is the namespace's own
// bind-time context. NULL on a bb_http_section_ns_t means PATCH/POST is
// unsupported for this namespace (the dispatcher returns 405 without calling
// anything). A bb_data-backed implementation of this hook drives
// bb_data_parse()/bb_data_commit() DIRECTLY (not bb_data_apply()) so the
// stage tag is authoritative rather than inferred.
typedef bb_http_section_apply_result_t (*bb_http_section_apply_fn)(const char *name,
                                                                    const char *body, size_t body_len,
                                                                    void *ctx);

// BB_ERR_UNSUPPORTED IS OVERLOADED ACROSS LAYERS -- the same code means
// three different things depending on where it's returned from:
//   - a namespace's apply() hook returning it at PARSE stage: "no apply
//     hook / unsupported wire format" -> default 405 (method-not-allowed
//     shaped)
//   - a namespace's apply() hook returning it at COMMIT stage: could mean
//     "this namespace doesn't support the write" (405, method-shaped) OR
//     "the backend genuinely lacks the capability" (501, a real
//     Not-Implemented) OR "the request shape itself isn't supported" (400,
//     client-shaped) depending on the namespace's own domain -- merged
//     precedent already disagrees: bb_storage_http's factory-reset route
//     maps BB_ERR_UNSUPPORTED -> 501 (storage backend genuinely lacks
//     erase_all), bb_wifi_http's PATCH route maps it -> 400 (unsupported
//     request shape). A single hardcoded 405 in the shared mapper cannot
//     serve all three -- see unsupported_status below.
// This is the SAME code-aliasing class PR #955 fixed one layer down (JSON
// parse failures used to alias BB_ERR_INVALID_STATE before that PR gave
// them a disjoint namespace) -- the override field exists because of it, so
// the next person who hits a "this bb_err_t means three things" bug knows
// the pattern and the precedent.
//
// One namespace's registration. `prefix` (e.g. "/api/sensors/") is BORROWED
// -- the caller (typically a composition root) keeps it alive for the
// process lifetime, a static const/string literal. `render`/`apply` are
// individually optional (NULL means that method is unsupported for this
// namespace -- 405); at least one MUST be set. `unsupported_status`
// overrides the HTTP status bb_http_section_status_for_apply() maps a
// commit-stage BB_ERR_UNSUPPORTED result to for THIS namespace: 0 (the
// zero value, so a plain-initialized bb_http_section_ns_t keeps today's
// behavior) means "use the mapper's own default, 405"; any other value
// (e.g. 501 for a genuine backend-capability gap, 400 for an
// unsupported-request-shape) is sent verbatim instead.
typedef struct {
    const char                 *prefix;
    bb_http_section_render_fn   render;
    bb_http_section_apply_fn    apply;
    void                        *ctx;
    int                          unsupported_status;
} bb_http_section_ns_t;

// Registers `ns`. Copies its fields (not the pointer) into the registry's
// own table -- `ns` itself may be a stack temporary, but every pointer field
// it carries (prefix/ctx) must remain valid for the process lifetime.
//
// First-registration wins: a duplicate `prefix` is REJECTED
// (BB_ERR_INVALID_STATE), never silently overridden -- mirrors
// bb_diag_register_section()'s own duplicate-name posture.
//
// MINIMUM SEGMENT DEPTH: a `prefix` with fewer than 2 non-empty path
// segments (e.g. "/api/", one segment) is REJECTED -- exactly the
// blanket-shadowing shape this dispatcher deliberately avoids (one wildcard
// route per registered namespace's own prefix, never a single blanket
// "/api/*" -- see bb_http_section_init()'s own doc). A prefix needs at
// least a segment past "/api/" of its own (e.g. "/api/sensors/", two
// segments) to be a real namespace rather than the whole API surface.
//
// Returns BB_ERR_INVALID_ARG if `ns` or `ns->prefix` is NULL, if both
// `ns->render` and `ns->apply` are NULL, if strlen(ns->prefix) >=
// BB_HTTP_SECTION_PREFIX_MAX, or if `ns->prefix` has fewer than 2
// non-empty path segments (see MINIMUM SEGMENT DEPTH above).
// Returns BB_ERR_INVALID_STATE if `ns->prefix` is already registered.
// Returns BB_ERR_NO_SPACE if the table is full (BB_HTTP_SECTION_TABLE_CAP
// distinct namespaces already registered).
bb_err_t bb_http_section_register_ns(const bb_http_section_ns_t *ns);

#ifdef ESP_PLATFORM
#include "bb_http_server.h"

// Registers a GET and a PATCH route (each "<ns->prefix>*", via
// bb_http_register_route -- 0 httpd slots, routed through the existing
// software /api/* dispatch table) for every namespace registered so far via
// bb_http_section_register_ns(). ESP-IDF only -- there is no host server to
// register against. Call once during composition, after every
// bb_http_section_register_ns() call.
bb_err_t bb_http_section_init(bb_http_handle_t server);

#endif /* ESP_PLATFORM */

#ifdef BB_HTTP_SECTION_TESTING
// Test-only: clears every registered namespace back to empty.
void bb_http_section_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
