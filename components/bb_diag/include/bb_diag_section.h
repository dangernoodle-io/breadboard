#pragma once

/**
 * @brief Generic section registry for the canonical GET /api/diag/<name>
 * wildcard route (B1-diag-dissolution PR3). A consumer registers a named
 * section once (typically from a composition root or the owning
 * component's own init fn) via bb_diag_register_section(); the ESP-IDF
 * dispatcher (bb_diag_sections_init(), bb_diag_http, platform/espidf/
 * bb_diag_http/bb_diag_http_section_dispatch.c) then serves GET /api/diag/<name> for every
 * registered section, streaming its snapshot straight through
 * bb_http_serialize_stream() (bb_serialize_json's flush-sink bridge, B1-1077
 * PR-1) -- NOT through bb_data. bb_diag stays bb_data-free here on purpose:
 * this registry is a smaller, more direct seam than bb_data's binding
 * table, with no wire-format lookup indirection beyond bb_serialize_json
 * itself (JSON only -- no format-registry dispatch).
 *
 * STREAMING TRADEOFF: because the response is streamed chunk-by-chunk, a
 * render failure that occurs after the first chunk is already on the wire
 * (for any reason other than a client disconnect) surfaces to the caller
 * as a truncated-200 body, never a 500 -- see
 * bb_serialize_json_stream_render()'s doc comment (bb_serialize_json.h)
 * for the full tradeoff. Accepted for every section registered here.
 *
 * This PR ships the MECHANISM ONLY -- zero real section names. Producers
 * (meminfo/system/mdns/storage) land in later PRs against this same API.
 *
 * A registered name derives its route as GET /api/diag/<name>. Exact
 * routes (the 12 legacy handlers still owned by bb_diag_http's own
 * bb_diag_routes_init() and sibling *_routes components) continue to win
 * over this wildcard for as long as they coexist -- see bb_http_server's
 * exact-before-wildcard dispatch contract (PR2, #937).
 */

#include "bb_core.h"
#include "bb_serialize.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed table capacity -- a handful of composed sections, not an
// open-ended runtime set (mirrors bb_data's own small-registry sizing
// rationale). No Kconfig bridge -- this is a compile-time-only constant.
#define BB_DIAG_SECTION_TABLE_CAP 16

// Max length (including the terminating NUL) of a section name -- the
// "<name>" segment of "/api/diag/<name>". bb_diag_register_section()
// rejects (BB_ERR_INVALID_ARG) any name whose strlen() is >= this bound
// rather than silently truncating it.
#define BB_DIAG_SECTION_NAME_MAX 24

// Args passed to a section's fill hook on every request. bb_diag-LOCAL
// type (deliberately NOT bb_data_gather_args_t) -- bb_diag never includes
// bb_data.h. Byte-shape mirrors bb_data_gather_args_t so wrapping a future
// bb_data_gather_fn as a section fill_fn (or vice versa) is a one-line
// adapter, not a redesign.
typedef struct {
    void                       *ctx;
    const bb_serialize_query_t *query;  // NULL if the section declares no query_keys
} bb_diag_fill_args_t;

// Fill hook: fills `dst` (the section's snap_desc's snap_size bytes,
// CALLER-OWNED scratch storage) from live sources. Same contract as
// bb_data_gather_fn: `args` is never NULL; `args->query` may be NULL.
typedef bb_err_t (*bb_diag_fill_fn)(void *dst, const bb_diag_fill_args_t *args);

// Two-phase iter hook -- the BB_ARR_STREAM alternative to `fill`, for a
// section whose snap_desc declares exactly one top-level BB_TYPE_ARR field
// with `.cardinality == BB_ARR_STREAM` (reg-time-enforced, see
// bb_diag_register_section() below). Driven around ONE request by the
// ESP-IDF dispatcher (platform/espidf/bb_diag_http/bb_diag_http_section_dispatch.c),
// which owns the row arena -- the section never allocates it:
//
//   Phase 1 COUNT: row_arena == NULL, row_cap == 0 -> fill `dst`'s scalar
//   fields (everything except the STREAM field itself) and report the true
//   row count via `*row_count`; write NO rows (there is no arena yet).
//
//   Phase 2 FILL: row_arena = the dispatcher's arena (row_cap elements,
//   each the STREAM field's elem_size bytes, sized from phase 1's count) ->
//   fill up to row_cap rows into row_arena, re-derive `dst`'s scalar fields,
//   and wire `dst`'s carrier field to the arena (typically via
//   bb_serialize_arr_stream_from_buf(), using iterator state that lives
//   inside `dst` itself so it survives the walk that follows this call).
//   Reports the actual row count written via `*row_count` -- may be less
//   than row_cap (a source that shrank between phase 1 and phase 2), never
//   more.
typedef bb_err_t (*bb_diag_iter_fn)(void *dst, void *row_arena, size_t row_cap,
                                     size_t *row_count, const bb_diag_fill_args_t *args);

// One section's registration. `snap_desc` is BORROWED -- the caller (the
// producer component) keeps it alive for the process lifetime, typically a
// static const. `name`/`desc`/`query_keys` strings must likewise outlive
// registration (static const/string literals).
typedef struct {
    const char                *name;       // derives GET /api/diag/<name>; strlen() < BB_DIAG_SECTION_NAME_MAX
    const char                *desc;       // human-readable; not wired to bb_openapi in this PR
    const bb_serialize_desc_t *snap_desc;  // BORROWED SSOT
    bb_diag_fill_fn             fill;      // XOR iter -- exactly one set
    bb_diag_iter_fn             iter;      // XOR fill -- exactly one set
    void                       *ctx;
    const char *const         *query_keys;   // NULL-ok
    size_t                      n_query_keys; // <= BB_SERIALIZE_QUERY_MAX_PARAMS (4)

    // Nullable; logically `const bb_route_t *` (bb_http.h) -- opaque `const
    // void *` here because bb_diag stays bb_http-free (B1-1153's HTTP-free
    // split; see this component's CMakeLists.txt). PRODUCER-OWNED
    // static-const describe-only route (handler=NULL, .rodata/flash, never
    // DRAM) -- except a producer opting into CONFIG_BB_OPENAPI_RUNTIME_META,
    // which patches .schema once at init (mutable .data) before serving --
    // makes GET /api/diag/<name> visible to bb_openapi_emit()
    // without registering a live handler. The ESP-IDF dispatcher
    // (bb_diag_sections_init(), platform/espidf/bb_diag_http/
    // bb_diag_http_section_dispatch.c, which DOES see bb_http_server.h)
    // casts this back to `const bb_route_t *` and passes it straight to
    // bb_http_register_route_descriptor_only() -- no copy, no mutable
    // per-section state.
    const void                *describe_route;
} bb_diag_section_t;

// Registers `section`. Copies its fields (not the pointer) into the
// registry's own table -- `section` itself may be a stack temporary, but
// every pointer field it carries (name/desc/snap_desc/fill/iter/ctx/query_keys)
// must remain valid for the process lifetime.
//
// First-registration wins: a duplicate `name` is REJECTED
// (BB_ERR_INVALID_STATE), never silently overridden -- two producers
// claiming the same section name is a composition bug (unlike bb_data_bind's
// intentional override-on-rebind semantics, which serves a different,
// disjoint-producer use case).
//
// Exactly one of `fill`/`iter` must be set (both or neither is
// BB_ERR_INVALID_ARG). A `fill` section is rejected (BB_ERR_INVALID_ARG) if
// any top-level snap_desc field has `.cardinality == BB_ARR_STREAM` -- fill
// only ever populates a caller-owned scratch snapshot in one shot, with no
// two-phase arena to wire a STREAM carrier against. An `iter` section's
// snap_desc must declare EXACTLY ONE top-level BB_TYPE_ARR field with
// `.cardinality == BB_ARR_STREAM` (else BB_ERR_INVALID_ARG); that field's
// elem_type must be BB_TYPE_OBJ (else BB_ERR_INVALID_ARG), its elem_size
// must be NONZERO (else BB_ERR_INVALID_ARG -- an unset elem_size is an
// authoring mistake, not a legitimate zero-byte row) and must fit
// BB_SERIALIZE_MAX_ROW_BYTES (else BB_ERR_NO_SPACE, mirroring the
// snap_size-exceeds-scratch reject below) -- all checked here, once, at
// registration time, rather than per request.
//
// Returns BB_ERR_INVALID_ARG if `section`, `section->name`, or
// `section->snap_desc` is NULL; if `section->fill`/`section->iter` are both
// set or both NULL; if strlen(section->name) >= BB_DIAG_SECTION_NAME_MAX;
// if section->n_query_keys > BB_SERIALIZE_QUERY_MAX_PARAMS; or if the
// fill/iter stream-field validation above fails.
// Returns BB_ERR_INVALID_STATE if `section->name` is already registered.
// Returns BB_ERR_NO_SPACE if the table is full (BB_DIAG_SECTION_TABLE_CAP
// distinct names already registered), if
// section->snap_desc->snap_size exceeds CONFIG_BB_DIAG_SECTION_SCRATCH_BYTES
// (the shared scratch buffer every dispatched request renders into), or if
// an `iter` section's stream field's elem_size exceeds
// BB_SERIALIZE_MAX_ROW_BYTES -- all loud, attach-time rejects rather than a
// silent per-request truncation.
bb_err_t bb_diag_register_section(const bb_diag_section_t *section);

// bb_diag_sections_init() (the GET /api/diag/* wildcard dispatcher)
// relocated to bb_diag_http (B1-1153, KB 1477) -- see
// components/bb_diag_http/include/bb_diag_http.h. bb_diag itself is
// bb_http_server-free after this split.

#ifdef BB_DIAG_SECTION_TESTING
// Test-only: clears every registered section back to empty.
void bb_diag_section_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
