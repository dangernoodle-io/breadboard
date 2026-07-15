#pragma once

/**
 * @brief Format-dispatch registry -- a name-keyed bb_registry instance
 * (keyed by bb_format_name(fmt)) mapping a bb_format_t to the emit-vtable
 * (and opaque ingest handle) a format backend registers for it.
 *
 * bb_serialize itself has no knowledge of any wire format -- a format
 * backend (e.g. bb_serialize_json, in its own component that REQUIRES
 * bb_serialize) builds a static const bb_serialize_format_entry_t and calls
 * bb_serialize_format_register() once, typically from a `// bbtool:init
 * tier=early` marker. A consumer that wants a format's emit vtable or
 * ingest handle at runtime looks it up by bb_format_t via
 * bb_serialize_format_get_emit()/bb_serialize_format_get_parse() instead of
 * hardcoding a #include of that format's header -- the seam this PR adds,
 * consumed starting in a later PR (bb_cache_serialize's hardcoded
 * BB_FORMAT_JSON branch is untouched here).
 *
 * `parse` is deliberately an opaque `const void *` -- this registry has no
 * knowledge of any format's ingest-vtable type (bb_serialize_json's, or any
 * future format's, may differ in shape). A caller that looks up `parse`
 * casts it back to the type it knows the registered format actually uses.
 */

#include "bb_core.h"
#include "bb_format.h"
#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

// One format backend's registered vtables. `emit` is the
// bb_serialize_emit_t implementation (borrowed, must outlive the
// registration -- typically a file-scope static const in the backend's own
// component). `parse` is an opaque pointer to the backend's own ingest
// vtable/handle (e.g. a `const bb_serialize_json_ingest_t *`, or NULL if the
// backend is emit-only); the registry never dereferences it.
typedef struct {
    const bb_serialize_emit_t *emit;
    const void                *parse;
} bb_serialize_format_entry_t;

// Registers `entry` under `fmt`. Returns BB_ERR_INVALID_ARG if `fmt` is
// BB_FORMAT_NONE, out of range, or `entry` is NULL. Re-registering the same
// `fmt` with an identical `entry` (same `emit`/`parse` pointers) is a no-op
// (BB_OK) -- the legitimate idempotent codegen re-run. Re-registering with a
// *different* `entry` is rejected (BB_ERR_INVALID_STATE, logged) rather than
// silently overwritten -- a different backend claiming an already-registered
// format is a composition bug, NOT last-writer-wins.
bb_err_t bb_serialize_format_register(bb_format_t fmt, const bb_serialize_format_entry_t *entry);

// Returns the emit vtable registered for `fmt`, or NULL if `fmt` has no
// registered entry (including BB_FORMAT_NONE/out-of-range, which can never
// be registered).
const bb_serialize_emit_t *bb_serialize_format_get_emit(bb_format_t fmt);

// Returns the opaque parse handle registered for `fmt`, or NULL if `fmt`
// has no registered entry, or the registered entry's `parse` was itself
// NULL (emit-only backend).
const void *bb_serialize_format_get_parse(bb_format_t fmt);

#ifdef BB_SERIALIZE_FORMAT_TESTING
// Clears every registered entry. Host-test teardown only -- lets each test
// case exercise "nothing registered for this format" independent of
// registration order across the file.
void bb_serialize_format_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
