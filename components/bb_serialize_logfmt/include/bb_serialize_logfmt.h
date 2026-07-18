#pragma once

/**
 * @brief Hand-rolled, no-heap, bounded-buffer logfmt bb_serialize_emit_t
 * backend -- a second wire-format implementation for bb_serialize, mirroring
 * bb_serialize_json's structure and contract shape.
 *
 * bb_serialize_logfmt_render() drives bb_serialize_walk() against a caller
 * descriptor+snapshot pair and writes a single `key=value key=value` logfmt
 * line directly into a caller-owned fixed buffer. All-or-nothing, same as
 * bb_serialize_json_render(): on any overflow the whole call fails
 * (BB_ERR_NO_SPACE) rather than returning a truncated line. No heap, no
 * snprintf, no locale-dependent formatting -- suitable for constrained
 * (no-PSRAM) targets and hot paths.
 *
 * Value quoting follows a logfmt-style convention: a value is wrapped in
 * double quotes and escaped if it is empty, or contains a space, `"`, `=`,
 * `\`, or any control byte (< 0x20) -- see bb_serialize_logfmt.c's
 * bb_logfmt_needs_quote() for the exact predicate. Backslash is deliberately
 * always quoted/escaped here (stricter than go-logfmt) to ensure unambiguous
 * round-tripping. An unquoted value therefore never needs escaping.
 * Nested BB_TYPE_OBJ/BB_TYPE_ARR fields are supported structurally (begin/end
 * are safe no-ops) but, like bb_serialize_console, this backend does not
 * prefix a nested field's key with its parent's key -- it targets flat,
 * scalar-only descriptors.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_core.h"
#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

// Writer state -- caller-owned, no heap. `buf`/`cap` are supplied by the
// caller via bb_serialize_logfmt_ctx_init(); `cap` is the usable capacity
// (bb_serialize_logfmt_render() reserves the final byte for the NUL
// terminator internally before initializing this context).
typedef struct {
    char    *buf;
    size_t   cap;   // usable capacity (excludes any reserved NUL byte)
    size_t   len;   // bytes written so far, excludes NUL
    bb_err_t err;   // sticky; BB_OK until the first overflow
} bb_serialize_logfmt_ctx_t;

// Initializes `ctx` to an empty writer over `buf`/`cap`. Does not write
// anything -- use bb_serialize_logfmt_render() for the one-shot
// descriptor-driven entry point, or drive bb_serialize_logfmt_emit()
// directly for manual control.
void bb_serialize_logfmt_ctx_init(bb_serialize_logfmt_ctx_t *ctx, char *buf, size_t cap);

// Returns a bb_serialize_emit_t vtable bound to `ctx` (format_id ==
// BB_FORMAT_LOGFMT). Pass the result to bb_serialize_walk().
bb_serialize_emit_t bb_serialize_logfmt_emit(bb_serialize_logfmt_ctx_t *ctx);

// Registers this backend under BB_FORMAT_LOGFMT in bb_serialize's
// format-dispatch registry (bb_serialize_format_register(), see
// bb_serialize_format.h) so a runtime consumer can look up the logfmt render
// fn by bb_format_t rather than #include-ing this header directly. The
// registered render fn is bb_serialize_logfmt_render() itself -- a one-shot,
// self-contained fn a caller looking it up via
// bb_serialize_format_get_render() calls directly with its own
// desc/snap/buf/cap/out_len, no per-call ctx copy/rebind needed. The
// registered parse handle is NULL (render-only backend, no ingest side).
// Idempotent (last-writer-wins, per bb_serialize_format_register()) -- safe
// to call more than once.
// bbtool:init tier=early fn=bb_serialize_logfmt_register_format
bb_err_t bb_serialize_logfmt_register_format(void);

// One-shot entry point: walks `desc`/`snap` and writes a complete logfmt
// line into `buf` (capacity `cap`, including room for the NUL terminator).
// All-or-nothing: on success, `*out_len` is the written length (excluding
// NUL) and `buf` is NUL-terminated; on BB_ERR_NO_SPACE, `*out_len` is 0 and
// `buf[0]` is '\0' -- never a partial line. Returns BB_ERR_NO_SPACE without
// touching `buf` if `cap` is 0.
bb_err_t bb_serialize_logfmt_render(const bb_serialize_desc_t *desc, const void *snap,
                                     char *buf, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
