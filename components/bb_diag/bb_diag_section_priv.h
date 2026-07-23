#pragma once
// Private shared header for the bb_diag section registry (B1-diag-dissolution
// PR3). Kconfig-bridge byte budgets plus the pure, portable dispatch helpers
// (name-from-uri parsing, query-param threading) -- factored out of the
// ESP-IDF-only dispatcher (platform/espidf/bb_diag_http/bb_diag_http_section_dispatch.c)
// so both it and a host test can exercise the SAME code path (mirrors PR2's
// bb_http_uri_strip_query_copy: the shared logic lives in a portable file,
// the ESP-IDF backend is a thin caller). No ESP-IDF or FreeRTOS types here.
// Included by:
//   - components/bb_diag/bb_diag_section.c
//   - platform/espidf/bb_diag_http/bb_diag_http_section_dispatch.c
//   - test/test_host/test_bb_diag_section.c

#include "bb_diag_section.h"

#include <stdbool.h>
#include <stddef.h>

// Shared per-request scratch buffer size -- the render path's snapshot
// scratch. bb_diag_register_section() rejects (BB_ERR_NO_SPACE) any section
// whose snap_desc->snap_size exceeds this at registration time, so a
// dispatched request can never overrun the buffer it renders into.
#ifdef CONFIG_BB_DIAG_SECTION_SCRATCH_BYTES
#define BB_DIAG_SECTION_SCRATCH_BYTES CONFIG_BB_DIAG_SECTION_SCRATCH_BYTES
#else
#define BB_DIAG_SECTION_SCRATCH_BYTES 512
#endif

// Per-query-value stack buffer size used while threading a section's
// declared query_keys through to its fill hook.
#ifdef CONFIG_BB_DIAG_SECTION_QUERY_VALUE_BYTES
#define BB_DIAG_SECTION_QUERY_VALUE_BYTES CONFIG_BB_DIAG_SECTION_QUERY_VALUE_BYTES
#else
#define BB_DIAG_SECTION_QUERY_VALUE_BYTES 32
#endif

// Bounds the dispatcher's stack buffer for a request's stripped URI path --
// "/api/diag/" (10 bytes) plus a name up to BB_DIAG_SECTION_NAME_MAX - 1
// bytes, plus NUL. Rounded up for headroom. Compile-time-only, not a
// per-target Kconfig tunable.
#define BB_DIAG_SECTION_URI_MAX (16 + BB_DIAG_SECTION_NAME_MAX)

// Looks up a registered section by name. Returns NULL if `name` is NULL or
// not registered.
const bb_diag_section_t *bb_diag_section_find(const char *name);

// Returns the number of sections currently registered via
// bb_diag_register_section(). Used by the ESP-IDF dispatcher
// (bb_diag_sections_init(), platform/espidf/bb_diag_http/
// bb_diag_http_section_dispatch.c) to walk the table and describe each
// section with a schema to the OpenAPI route registry (B1-1180 PR-1) --
// mirrors bb_http_section_count()'s sibling precedent
// (components/bb_http_server/src/bb_http_section_priv.h).
size_t bb_diag_section_count(void);

// Returns section `idx`'s registration (idx MUST be < bb_diag_section_count()).
// Registration order is stable: bb_diag_register_section() claims slots via
// first-free-slot allocation and this registry supports no removal, so
// slots [0, bb_diag_section_count()) are always exactly the in-use ones, in
// registration order.
const bb_diag_section_t *bb_diag_section_at(size_t idx);

// Callback invoked once per registered section carrying a non-NULL
// `describe_route` (bb_diag_section_t.describe_route) -- `describe_route`
// is that same opaque pointer, logically `const bb_route_t *` (bb_http.h);
// the caller (an ESP-IDF-only TU that DOES see bb_http_server.h) casts it
// back and hands it to bb_http_register_route_descriptor_only(). Returns
// whatever that registration call returns; a non-BB_OK return does not stop
// the walk (see bb_diag_section_describe_foreach()).
typedef bb_err_t (*bb_diag_describe_register_fn)(const void *describe_route, void *ctx);

// Walks every registered section (bb_diag_section_at(), registration
// order) and calls `register_fn(sec->describe_route, ctx)` for each one
// whose `describe_route` is non-NULL -- the ENTIRE selection logic behind
// the ESP-IDF dispatcher's OpenAPI describe step (B1-1180 PR-1,
// bb_diag_sections_init(), platform/espidf/bb_diag_http/
// bb_diag_http_section_dispatch.c), factored out here so a host test can
// drive the SAME code path with a fake `register_fn` (no bb_http_server
// dependency needed to prove the walk/selection is correct). Every
// producer's `describe_route` is itself a `static const` route owned by
// that producer (.rodata/flash) -- this helper never allocates, copies, or
// mutates any per-section state of its own.
//
// Continue-on-error: a failing `register_fn` call does not stop the walk
// (mirrors the dispatcher's prior per-section log-and-continue behavior) --
// every section with a non-NULL `describe_route` is always attempted.
// Returns the LAST non-BB_OK result seen (BB_OK if every call succeeded, or
// if no section had a describe_route). Returns BB_ERR_INVALID_ARG if
// `register_fn` is NULL.
bb_err_t bb_diag_section_describe_foreach(bb_diag_describe_register_fn register_fn, void *ctx);

// Reads the cached stream-field element size for `sec` -- a section pointer
// previously returned by bb_diag_section_find() (i.e. `&slot->section` for
// some registered slot; the accessor recovers the owning slot from `sec`'s
// own address). Cached at registration time by bb_diag_register_section()'s
// iter-section stream-field validation, so the ESP-IDF dispatcher can size
// its row arena (`row_count * elem_size`) without re-scanning `sec->snap_desc`
// on every request. Returns 0 for a fill-based section (unused there) or a
// NULL `sec`.
size_t bb_diag_section_stream_elem_size(const bb_diag_section_t *sec);

// Extracts the "<name>" segment from a request URI of the form
// "/api/diag/<name>" (query string, if any, already stripped by the
// caller -- same contract as bb_http_req_uri()) into `out` (capacity
// `out_cap`), NUL-terminated, truncated if it doesn't fit.
//
// Returns BB_ERR_INVALID_ARG if `uri`, `out` is NULL, or `out_cap` is 0.
// Returns BB_ERR_NOT_FOUND if `uri` does not start with "/api/diag/".
bb_err_t bb_diag_section_name_from_uri(const char *uri, char *out, size_t out_cap);

// Query-param getter abstraction -- decouples bb_diag_section_build_query()
// from any HTTP type, so it (and every caller of it) stays portable/
// host-testable. `ctx` is opaque, forwarded byte-for-byte to `get`. Returns
// true if `key` was found (value copied into `out`, capacity `out_cap`,
// NUL-terminated, truncated if it doesn't fit); false if `key` is absent
// (`out` left untouched).
typedef bool (*bb_diag_query_getter_fn)(void *ctx, const char *key, char *out, size_t out_cap);

// Builds a bb_serialize_query_t from `sec->query_keys` by calling `get`
// once per declared key. A key the getter reports absent is simply omitted
// from `*out` (not an error) -- `out->count` reflects however many were
// actually found. `value_scratch` is CALLER-OWNED storage of at least
// `sec->n_query_keys * BB_DIAG_SECTION_QUERY_VALUE_BYTES` bytes; found
// value i is written into `value_scratch + i * BB_DIAG_SECTION_QUERY_VALUE_BYTES`
// and `out->params[i].value` points there.
//
// Returns BB_ERR_INVALID_ARG if `sec`, `get`, `value_scratch`, or `out` is
// NULL. A section with `n_query_keys == 0` is not an error -- `out->count`
// is simply set to 0 (`get` is never called).
bb_err_t bb_diag_section_build_query(const bb_diag_section_t *sec,
                                      bb_diag_query_getter_fn get, void *get_ctx,
                                      char *value_scratch, bb_serialize_query_t *out);
