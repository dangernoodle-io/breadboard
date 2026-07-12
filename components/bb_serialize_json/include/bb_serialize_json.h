#pragma once

/**
 * @brief Hand-rolled, no-heap, bounded-buffer JSON bb_serialize_emit_t
 * backend -- the default wire-format implementation for bb_serialize.
 *
 * bb_serialize_json_render() drives bb_serialize_walk() against a caller
 * descriptor+snapshot pair and writes JSON directly into a caller-owned
 * fixed buffer. All-or-nothing: on any overflow the whole call fails
 * (BB_ERR_NO_SPACE) rather than returning truncated/partial JSON. No heap,
 * no snprintf, no locale-dependent formatting -- suitable for constrained
 * (no-PSRAM) targets and hot paths.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_core.h"
#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

// One container-nesting level of writer state: whether the level is a JSON
// array (unkeyed elements) or object (keyed members), and whether a prior
// sibling has already been written (drives the leading-comma decision).
typedef struct {
    bool is_array;
    bool have_child;
} bb_serialize_json_level_t;

// Writer state -- caller-owned, no heap. `buf`/`cap` are supplied by the
// caller via bb_serialize_json_ctx_init(); `cap` is the FULL buffer
// capacity as given -- bb_serialize_json_render() reserves the final byte
// for the NUL terminator internally before initializing this context.
typedef struct {
    char    *buf;
    size_t   cap;    // usable capacity (excludes any reserved NUL byte)
    size_t   len;    // bytes written so far, excludes NUL
    bb_err_t err;     // sticky; BB_OK until the first overflow
    uint8_t  depth;   // index of the current (top) level in `stack`
    // A walker recursion level can cost UP TO TWO JSON containers, not one:
    // a BB_TYPE_ARR field with elem_type == BB_TYPE_OBJ opens an array
    // frame (begin_arr) AND, per element, an object frame (begin_obj)
    // before the walker's depth guard advances -- so a chain of nested
    // arr-of-obj fields costs 2 stack frames per BB_SERIALIZE_MAX_DEPTH
    // level. Sized 2*BB_SERIALIZE_MAX_DEPTH+2 (+2 for the root frame plus
    // slack) to cover that worst case; bb_json_push_level() also enforces
    // this bound at runtime (BB_ERR_NO_SPACE, no write) so a future change
    // to either constant can't silently reintroduce an OOB write.
    bb_serialize_json_level_t stack[2 * BB_SERIALIZE_MAX_DEPTH + 2];
} bb_serialize_json_ctx_t;

// Initializes `ctx` to an empty writer over `buf`/`cap`. Does not write
// anything (no opening brace) -- use bb_serialize_json_render() for the
// one-shot descriptor-driven entry point, or drive bb_serialize_json_emit()
// directly for manual control.
void bb_serialize_json_ctx_init(bb_serialize_json_ctx_t *ctx, char *buf, size_t cap);

// Returns a bb_serialize_emit_t vtable bound to `ctx` (format_id ==
// BB_FORMAT_JSON). Pass the result to bb_serialize_walk().
bb_serialize_emit_t bb_serialize_json_emit(bb_serialize_json_ctx_t *ctx);

// One-shot entry point: walks `desc`/`snap` and writes a complete JSON
// object (wrapped in `{`...`}`) into `buf` (capacity `cap`, including room
// for the NUL terminator). All-or-nothing: on success, `*out_len` is the
// written length (excluding NUL) and `buf` is NUL-terminated; on
// BB_ERR_NO_SPACE, `*out_len` is 0 and `buf[0]` is '\0' -- never partial
// JSON.
bb_err_t bb_serialize_json_render(const bb_serialize_desc_t *desc, const void *snap,
                                   char *buf, size_t cap, size_t *out_len);

// Same as bb_serialize_json_render(), plus BB_TYPE_REF resolution: drives
// bb_serialize_walk_ref() (rather than bb_serialize_walk()) with `resolve`/
// `resolve_ctx`, so a REF field's sibling section renders inline at its
// wire key. All-or-nothing semantics and the NUL-terminator/overflow
// contract are identical to bb_serialize_json_render().
bb_err_t bb_serialize_json_render_ref(const bb_serialize_desc_t *desc, const void *snap,
                                       char *buf, size_t cap, size_t *out_len,
                                       bb_serialize_ref_resolve_fn resolve, void *resolve_ctx);

// Computes a worst-case (upper-bound) byte count for rendering `desc` as
// JSON via bb_serialize_json_render() -- a sizing helper for callers that
// need to allocate/reserve a buffer statically. Returns SIZE_MAX if any
// subfield's width is unbounded (a BB_TYPE_STR_N or BB_TYPE_ARR field whose
// max_len/max_items hint is left at 0), OR if any subfield is a BB_TYPE_REF
// (a REF's rendered size depends on the resolver's sibling descriptor, which
// isn't known statically here -- always contributes an unbounded/unknown
// estimate).
size_t bb_serialize_json_bound(const bb_serialize_desc_t *desc);

#ifdef __cplusplus
}
#endif
