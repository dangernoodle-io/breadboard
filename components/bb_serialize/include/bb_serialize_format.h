#pragma once

/**
 * @brief Format-dispatch registry -- a name-keyed bb_registry instance
 * (keyed by bb_format_name(fmt)) mapping a bb_format_t to the one-shot
 * render fn (and opaque ingest handle) a format backend registers for it.
 *
 * bb_serialize itself has no knowledge of any wire format -- a format
 * backend (e.g. bb_serialize_json, in its own component that REQUIRES
 * bb_serialize) builds a static const bb_serialize_format_entry_t and calls
 * bb_serialize_format_register() once, typically from a `// bbtool:init
 * tier=early` marker. A consumer that wants a format's render fn or ingest
 * handle at runtime looks it up by bb_format_t via
 * bb_serialize_format_get_render()/bb_serialize_format_get_parse() instead
 * of hardcoding a #include of that format's header -- consumed starting
 * with bb_cache_serialize (PR2), which dispatches its render call through
 * this registry rather than a hardcoded BB_FORMAT_JSON branch.
 *
 * `render` is a self-contained, one-shot fn -- unlike an emit-vtable
 * template, it takes no ctx and needs no per-call copy/rebind: a consumer
 * calls it directly with desc/snap/buf/cap and gets back a complete render.
 * This structurally rules out the "forgot to bind a fresh ctx before
 * driving the vtable" footgun a template-based design would otherwise
 * invite.
 *
 * `parse` is a typed `bb_serialize_parse_fn`: it decodes a caller-owned
 * `buf[len]` using a caller-owned `scratch[scratch_cap]` (which must outlive
 * the subsequent populate call -- it backs the bound
 * bb_serialize_populate_t's own state) and yields a bound
 * bb_serialize_populate_t source in `*out_src`, which the caller then drives
 * via bb_serialize_populate(desc, dst, out_src). NULL means the backend is
 * render-only (no ingest side) -- same no-op contract as before.
 */

#include "bb_core.h"
#include "bb_format.h"
#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-shot render fn: walks `desc`/`snap` and writes a complete rendering
// into `buf` (capacity `cap`), writing the rendered length (excluding any
// format-specific terminator) to `*out_len`. Same signature/contract shape
// as bb_serialize_json_render() -- all-or-nothing, no partial output on
// failure.
typedef bb_err_t (*bb_serialize_render_fn)(const bb_serialize_desc_t *desc, const void *snap,
                                            char *buf, size_t cap, size_t *out_len);

// One-shot parse-dispatch fn: decodes `buf` (length `len`) using
// caller-owned `scratch` (capacity `scratch_cap`) and binds a
// bb_serialize_populate_t source into `*out_src`. `scratch` MUST outlive the
// caller's subsequent bb_serialize_populate(desc, dst, out_src) call -- it
// backs the returned source's own internal state (e.g. a format's token
// pool/recorder). Returns BB_ERR_NO_SPACE if `scratch_cap` is too small to
// lay out that state; otherwise the format's own bb_err_t (e.g. a parse/
// grammar error propagated from decoding `buf`), or BB_OK on success.
typedef bb_err_t (*bb_serialize_parse_fn)(const char *buf, size_t len,
                                           void *scratch, size_t scratch_cap,
                                           bb_serialize_populate_t *out_src);

// One format backend's registered entry. `render` is the backend's one-shot
// render fn (borrowed, must outlive the registration -- typically a
// file-scope static fn in the backend's own component). `parse` is the
// backend's own one-shot parse-dispatch fn (see bb_serialize_parse_fn above),
// or NULL if the backend is render-only.
typedef struct {
    bb_serialize_render_fn render;
    bb_serialize_parse_fn  parse;
} bb_serialize_format_entry_t;

// Registers `entry` under `fmt`. Returns BB_ERR_INVALID_ARG if `fmt` is
// BB_FORMAT_NONE, out of range, or `entry` is NULL. Re-registering the same
// `fmt` with an identical `entry` (same `render`/`parse` pointers) is a
// no-op (BB_OK) -- the legitimate idempotent codegen re-run. Re-registering
// with a *different* `entry` is rejected (BB_ERR_INVALID_STATE, logged)
// rather than silently overwritten -- a different backend claiming an
// already-registered format is a composition bug, NOT last-writer-wins.
bb_err_t bb_serialize_format_register(bb_format_t fmt, const bb_serialize_format_entry_t *entry);

// Returns the render fn registered for `fmt`, or NULL if `fmt` has no
// registered entry (including BB_FORMAT_NONE/out-of-range, which can never
// be registered).
bb_serialize_render_fn bb_serialize_format_get_render(bb_format_t fmt);

// Returns the parse fn registered for `fmt`, or NULL if `fmt` has no
// registered entry, or the registered entry's `parse` was itself NULL
// (render-only backend).
bb_serialize_parse_fn bb_serialize_format_get_parse(bb_format_t fmt);

// Convenience wrapper: looks up `fmt`'s registered render fn and calls it
// with `desc`/`snap`/`buf`/`cap`/`out_len`. Returns BB_ERR_UNSUPPORTED if
// `fmt` has no registered entry -- a simpler entry point than
// bb_serialize_format_get_render() + a manual NULL-check for a consumer
// that just wants "render this format" and doesn't need to hold the fn
// pointer across other work (e.g. bb_cache_serialize instead calls
// bb_serialize_format_get_render() directly, since its unsupported-format
// gate and its render call happen at different points in an already-locked
// flow).
bb_err_t bb_serialize_format_render(bb_format_t fmt, const bb_serialize_desc_t *desc, const void *snap,
                                     char *buf, size_t cap, size_t *out_len);

#ifdef BB_SERIALIZE_FORMAT_TESTING
// Clears every registered entry. Host-test teardown only -- lets each test
// case exercise "nothing registered for this format" independent of
// registration order across the file.
void bb_serialize_format_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
