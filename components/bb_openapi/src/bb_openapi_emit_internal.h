#pragma once
// Private interface for the bb_openapi device streaming emitter
// (bb_openapi_emit.c, ships in firmware). Previously also shared with an
// in-memory tree emitter (bb_openapi_emit(), removed in B1-1054 once the
// streaming path reached full parity); the plumbing that walks the
// route/schema registries and derives operationId/method strings still
// lives in bb_openapi_emit_shared.c and is declared here rather than
// duplicated. Also carries buffer-sizing constants shared with host tests,
// plus test-only accessors. Not for external consumers, not part of the
// public bb_openapi API surface (components/bb_openapi/include/bb_openapi.h).

#include "bb_openapi.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Method name helpers
// ---------------------------------------------------------------------------

const char *method_str(bb_http_method_t m);

// ---------------------------------------------------------------------------
// operationId derivation
// ---------------------------------------------------------------------------
// Rule: <method><PathCamelCase>
//   - strip leading '/'
//   - each path segment's first char is uppercased
//   - '-' and '_' are dropped and next char is uppercased
//   e.g. GET /api/stats -> "getApiStats"
//        POST /api/pool-config -> "postApiPoolConfig"

// Caller guarantees: non-NULL path (registry walkers skip NULL-path routes
// in collect_paths_walker / emit_operations_walker) and a non-empty buffer
// (only call site uses a fixed 128-byte stack array).
void derive_operation_id(bb_http_method_t method, const char *path,
                         char *out, size_t out_size);

// ---------------------------------------------------------------------------
// Path uniqueness tracking (stack array, no malloc)
// ---------------------------------------------------------------------------

#define UNIQUE_PATH_CAP 64

typedef struct {
    const char *paths[UNIQUE_PATH_CAP];
    size_t      count;
} path_set_t;

bool path_set_contains(const path_set_t *ps, const char *path);

// The bb_http registry caps at BB_ROUTE_REGISTRY_CAP (64) == UNIQUE_PATH_CAP,
// so the path_set can hold every distinct path the registry can store.
void path_set_add(path_set_t *ps, const char *path);

// Pass 1: collect unique paths.
// bb_http_register_described_route rejects NULL routes; descriptors stored in
// the registry are guaranteed non-NULL with non-NULL paths.
void collect_paths_walker(const bb_route_t *route, void *ctx);

// ---------------------------------------------------------------------------
// SSE oneOf fragment builder
// ---------------------------------------------------------------------------

// Per-$ref content budget — matches the existing ref_val[] sizing used to
// format "#/components/schemas/<component_name>".
#define BB_OPENAPI_SSE_REF_BUF_SIZE 80

// Sized for BB_OPENAPI_SCHEMA_REGISTRY_CAP entries at BB_OPENAPI_SSE_REF_BUF_SIZE
// each, which comfortably covers the realistic component-name lengths this
// registry actually holds (see bb_openapi.h). It intentionally does NOT add
// headroom on top of that for the per-entry `{"$ref":"...."}` wrapper syntax —
// build_sse_oneof_fragment() (bb_openapi_emit.c) detects and rejects the
// pathological case (many near-maximum-length component names at full
// registry cap) rather than truncate into malformed JSON.
#define BB_OPENAPI_SSE_ONEOF_BUF_SIZE \
    (sizeof("{\"oneOf\":[]}") + (BB_OPENAPI_SCHEMA_REGISTRY_CAP * BB_OPENAPI_SSE_REF_BUF_SIZE))

// Builds the complete `{"oneOf":[{"$ref":"..."},...]}` fragment for every
// registered schema with a non-NULL sse_topic. Returns true and leaves a
// NUL-terminated fragment in out on success; returns false (out left
// unspecified) if the fragment would not fit — caller must not splice out.
bool build_sse_oneof_fragment(char *out, size_t out_size);

// Count of registered schemas with a non-NULL sse_topic. Both emitters need
// this to decide whether to synthesize an SSE oneOf content block; a shared
// helper avoids each emitter re-deriving it via bb_openapi_schema_get() in a
// loop already bounded by bb_openapi_schema_count() (a redundant bounds
// re-check there is an untestable, permanently-missed branch — the count is
// only ever taken over the registry's own storage, directly, here).
size_t sse_schema_count(void);

#ifdef BB_OPENAPI_TESTING
// Test-only seam onto build_sse_oneof_fragment() (bb_openapi_emit_shared.c):
// builds the `{"oneOf":[{"$ref":"..."},...]}` fragment
// from the current schema registry into out (out_size bytes). Returns true
// and leaves a NUL-terminated fragment in out on success; returns false if
// the fragment (or even just the "{\"oneOf\":[" opener/"]}" closer) would
// not fit out_size — lets host tests drive each overflow branch directly
// with a caller-controlled out_size instead of contorting registry state
// to approach BB_OPENAPI_SCHEMA_REGISTRY_CAP-sized real data.
bool bb_openapi_build_sse_oneof_fragment_for_test(char *out, size_t out_size);
#endif /* BB_OPENAPI_TESTING */

#ifdef __cplusplus
}
#endif
