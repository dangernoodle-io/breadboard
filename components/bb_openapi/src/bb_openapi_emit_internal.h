#pragma once
// Private interface for bb_openapi_emit.c — buffer-sizing constants shared
// with host tests, plus test-only accessors. Not for external consumers,
// not part of the public bb_openapi API surface
// (components/bb_openapi/include/bb_openapi.h).

#include "bb_openapi.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef BB_OPENAPI_TESTING
// Test-only seam onto build_sse_oneof_fragment() (file-static in
// bb_openapi_emit.c): builds the `{"oneOf":[{"$ref":"..."},...]}` fragment
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
