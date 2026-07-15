#pragma once

/**
 * @brief Heap-over-serial emit backend -- a bb_serialize_emit_t
 * implementation that renders a snapshot as a single human-readable
 * "key=val key=val" line (no braces, no quoting), plus a one-shot
 * heap-snapshot report helper (bb_serialize_console_heap_report()) built on
 * top of bb_meminfo.
 *
 * Unlike bb_serialize_json's all-or-nothing overflow contract, this backend
 * uses snprintf-style truncation semantics: on overflow the line is
 * truncated but always NUL-terminated -- a clipped heap line logged over
 * serial is still useful, so a partial render is a success (BB_OK), never
 * BB_ERR_NO_SPACE.
 *
 * No heap, no task -- every call is a one-shot render into a caller- or
 * stack-owned buffer.
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
// caller via bb_serialize_console_ctx_init(); `cap` is the FULL buffer
// capacity, including room for the NUL terminator (every append respects
// this bound and never writes past it).
typedef struct {
    char  *buf;
    size_t cap;  // full capacity, including the NUL terminator
    size_t len;  // bytes written so far, excludes NUL
} bb_serialize_console_ctx_t;

// Initializes `ctx` to an empty writer over `buf`/`cap`. NUL-terminates
// `buf` immediately (an empty line) if cap > 0.
void bb_serialize_console_ctx_init(bb_serialize_console_ctx_t *ctx, char *buf, size_t cap);

// Returns a bb_serialize_emit_t vtable bound to `ctx` (format_id ==
// BB_FORMAT_CONSOLE). Pass the result to bb_serialize_walk(). Nested
// BB_TYPE_OBJ/BB_TYPE_ARR fields are supported structurally (begin/end
// callbacks are safe no-ops) but do NOT prefix child keys with their
// parent's key -- this backend is designed for flat, scalar-only
// descriptors (e.g. bb_serialize_console_heap_desc below); a descriptor
// with nested containers still renders without crashing, just without
// nesting-aware key qualification.
bb_serialize_emit_t bb_serialize_console_emit(bb_serialize_console_ctx_t *ctx);

// Registers this backend under BB_FORMAT_CONSOLE in bb_serialize's
// format-dispatch registry (bb_serialize_format_register(), see
// bb_serialize_format.h) so a runtime consumer can look up the console
// render fn by bb_format_t rather than #include-ing this header directly.
// The registered render fn is bb_serialize_console_render() itself; the
// registered parse handle is NULL (render-only backend, no ingest side).
// Idempotent (last-writer-wins, per bb_serialize_format_register()) -- safe
// to call more than once.
// bbtool:init tier=early fn=bb_serialize_console_register_format
bb_err_t bb_serialize_console_register_format(void);

// One-shot entry point: walks `desc`/`snap` and writes a single
// human-readable "key=val key=val" line into `buf` (capacity `cap`,
// including room for the NUL terminator). Always NUL-terminates `buf` (when
// cap > 0) and always returns BB_OK on a non-degenerate call -- overflow
// truncates the line rather than failing, per this backend's truncation
// contract (see the file header comment above). Returns BB_ERR_NO_SPACE
// without touching `buf` only when `buf` is NULL or `cap` is 0 (nothing can
// be written, not even a NUL terminator). `*out_len` (if non-NULL) is set to
// the actual number of bytes written (excluding NUL), which may be less
// than the untruncated length on overflow.
bb_err_t bb_serialize_console_render(const bb_serialize_desc_t *desc, const void *snap,
                                      char *buf, size_t cap, size_t *out_len);

// ---------------------------------------------------------------------------
// Heap-over-serial one-shot milestone report -- built on bb_meminfo (the
// canonical heap_caps reader SSOT, KB #698/#699/#693). No periodic task: a
// consumer calls bb_serialize_console_heap_report() at its own milestones
// (boot, post-httpd-init, ...).
// ---------------------------------------------------------------------------

// One heap snapshot, field-for-field a subset of bb_meminfo_snapshot_t
// (bb_meminfo.h) -- the fields most relevant to a quick serial glance.
// uint64_t (not size_t) because BB_TYPE_U64 fields are read as a raw
// uint64_t at their descriptor offset (see bb_serialize_walk()); this keeps
// the descriptor portable regardless of the platform's size_t width.
typedef struct {
    uint64_t internal_free;
    uint64_t internal_min_ever_free;
    uint64_t internal_largest_free_block;
    uint64_t spiram_free;
    uint64_t dma_free;
    uint64_t esp_min_free_heap;
} bb_serialize_console_heap_snap_t;

// Descriptor for bb_serialize_console_heap_snap_t -- the SSOT
// bb_serialize_console_heap_report() walks via bb_serialize_console_render().
extern const bb_serialize_desc_t bb_serialize_console_heap_desc;

// Populates `dst` (a bb_serialize_console_heap_snap_t*) from bb_meminfo_get().
// `ctx` is unused (pass NULL) -- present only to match a generic
// gather-fn shape for future reuse. Returns BB_ERR_INVALID_ARG if `dst` is
// NULL; otherwise propagates bb_meminfo_get()'s own return.
bb_err_t bb_serialize_console_heap_gather(void *dst, void *ctx);

// Gathers a fresh heap snapshot and logs it as a single line via bb_log_i(),
// prefixed with `label` (e.g. "boot", "post-httpd"). On-stack line buffer
// only (BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES, Kconfig-bridged), no heap. If
// the gather step fails, logs an error line and returns without rendering --
// never crashes.
void bb_serialize_console_heap_report(const char *label);

#ifdef __cplusplus
}
#endif
