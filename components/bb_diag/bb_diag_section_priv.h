#pragma once
// Private shared header for the bb_diag section registry (B1-diag-dissolution
// PR3). Kconfig-bridge byte budgets plus the pure, portable dispatch helpers
// (name-from-uri parsing, query-param threading) -- factored out of the
// ESP-IDF-only dispatcher (platform/espidf/bb_diag/bb_diag_section_dispatch.c)
// so both it and a host test can exercise the SAME code path (mirrors PR2's
// bb_http_uri_strip_query_copy: the shared logic lives in a portable file,
// the ESP-IDF backend is a thin caller). No ESP-IDF or FreeRTOS types here.
// Included by:
//   - components/bb_diag/bb_diag_section.c
//   - platform/espidf/bb_diag/bb_diag_section_dispatch.c
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

// Bounds the dispatcher's stack buffer for one rendered JSON response.
// Compile-time-only (not a per-target Kconfig tunable) -- mirrors floor's
// own FLOOR_DIAG_RENDER_BUF_SIZE non-Kconfig sizing convention
// (examples/floor/main/floor_app.c); raise if a future section's rendered
// output outgrows it.
#define BB_DIAG_SECTION_RENDER_BUF_BYTES 1024

// Looks up a registered section by name. Returns NULL if `name` is NULL or
// not registered.
const bb_diag_section_t *bb_diag_section_find(const char *name);

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
