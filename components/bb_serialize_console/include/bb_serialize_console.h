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
//
// `path`/`path_depth` is the nested-key qualification stack (B1-1416):
// begin_obj pushes the field's own `.key` onto `path`, end_obj pops it, and
// each scalar emit joins path[0..path_depth) plus the field's own key with
// '.' separators (e.g. "internal.free") before writing. Entries are
// BORROWED pointers into the walked descriptor's own static-const `.key`
// strings -- no copying, no allocation. Sized to bb_serialize.h's own
// BB_SERIALIZE_MAX_DEPTH (the walker refuses to recurse past that cap, see
// bb_serialize_walk.c), so a legitimate walk can never overflow this stack;
// a defensive guard still declines to push past the cap rather than
// corrupt the array (truncate-don't-fail, matching this backend's overall
// overflow contract).
typedef struct {
    char  *buf;
    size_t cap;  // full capacity, including the NUL terminator
    size_t len;  // bytes written so far, excludes NUL

    const char *path[BB_SERIALIZE_MAX_DEPTH];
    size_t      path_depth;
} bb_serialize_console_ctx_t;

// Initializes `ctx` to an empty writer over `buf`/`cap`. NUL-terminates
// `buf` immediately (an empty line) if cap > 0.
void bb_serialize_console_ctx_init(bb_serialize_console_ctx_t *ctx, char *buf, size_t cap);

// Returns a bb_serialize_emit_t vtable bound to `ctx` (format_id ==
// BB_FORMAT_CONSOLE). Pass the result to bb_serialize_walk(). BB_TYPE_OBJ
// nesting is qualification-aware (B1-1416): a scalar field nested inside
// one or more BB_TYPE_OBJ ancestors renders with its full dotted path,
// e.g. a field keyed "free" inside an object keyed "internal" renders as
// "internal.free=...", so sibling objects sharing a child key (e.g. two
// per-region heap snapshots that both have a "free" field) render as
// distinct tokens rather than colliding on one line.
//
// BB_TYPE_ARR is explicitly OUT OF SCOPE for qualification: the walker
// passes key == NULL for every array element (see bb_serialize_walk.c), so
// there is no parent key to compose with -- begin_arr/end_arr remain
// deliberate no-ops. B1-1414's one-line-per-row console render already
// owns the array/table story; this backend still renders an array's
// elements without crashing, just without nesting-aware qualification.
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

// TODO(B1-1024): superseded by bb_meminfo_heap_snap_t (components/bb_meminfo/
// bb_meminfo_heap_snap.h), the ONE format-agnostic heap snapshot per KB 1432.
// This console-local subset stays live for now -- bb_serialize_console_render()
// is flat/scalar-only (see the comment on bb_serialize_console_emit() above)
// and can't yet render bb_meminfo_heap_snap_t's nested per-region objects
// (BB_TYPE_OBJ children); that needs nested-key console render support
// (B1-1024). Retire this struct once that lands and
// bb_serialize_console_heap_report() walks bb_meminfo_heap_snap_desc
// directly.
//
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

// ---------------------------------------------------------------------------
// Task-stack-over-serial periodic report -- built on bb_task's base
// registry (components/bb_task, bb_task_base_foreach()), the SSOT every
// live FreeRTOS task's stack high-water free bytes is persisted to by the
// periodic base-scan (platform/espidf/bb_task_registry/
// bb_task_registry_base_scan.c), which already computes the value via
// bb_num_words_to_bytes() (B1-1128) -- this component reads that persisted
// value rather than sampling FreeRTOS a third time.
//
// One line PER TASK, not one nested report: bb_serialize_console_render()
// is flat/scalar-only (see the comment on bb_serialize_console_emit() above)
// and cannot render an array of task rows as a single structured line, so
// bb_serialize_console_tasks_report() walks a bounded snapshot and emits one
// bb_serialize_console_render() + bb_log_i() call per task (mirrors this
// header's own per-row precedent, e.g. bb_diag_tasks_get_wire's per-task
// rows, just logged instead of served).
// ---------------------------------------------------------------------------

// Independent of BB_TASK_NAME_MAX (bb_task.h) on purpose -- this public
// header stays free of a bb_task dependency (bb_task is a PRIV_REQUIRES,
// gather-implementation-only concern, see the .c file); the two constants
// happen to share the same value today because both mirror
// configMAX_TASK_NAME_LEN, not because one derives from the other.
#define BB_SERIALIZE_CONSOLE_TASKS_NAME_MAX 20

// One task's row: a subset of bb_task_base_entry_t (bb_task.h) plus the
// derived `used_bytes` -- the console line most consumers actually want
// ("how much of the budget is gone"), since free_bytes alone is easy to
// misread as "the WHOLE stack" rather than "what's left of it".
// uint64_t (not uint32_t) for the same portable-descriptor-width reason as
// bb_serialize_console_heap_snap_t above.
typedef struct {
    char     name[BB_SERIALIZE_CONSOLE_TASKS_NAME_MAX];
    // free_bytes is present-gated (see bb_serialize_console_tasks_desc) on
    // `sampled` below -- a task upserted since the last base-scan pass has
    // free_bytes==0 with sampled==false, and MUST render as unmeasured
    // rather than a false "0 bytes free" alarm (bb_task.h's
    // bb_task_base_entry_t.sampled is the SSOT for this bit).
    uint64_t free_bytes;
    // budget/used are present-gated (see bb_serialize_console_tasks_desc)
    // on stack_budget_bytes > 0 -- 0 means bb_task never learned this
    // task's budget (e.g. a third-party task, such as ESP-IDF's own
    // wifi_prov_mgr, that the base-scan discovered but that never went
    // through bb_task_create()/bb_task_registry_register()). used_bytes is
    // additionally gated on `sampled` (it's derived from free_bytes, so it
    // can't be trusted before free_bytes has ever been sampled either).
    uint64_t stack_budget_bytes;
    uint64_t used_bytes;
    // Mirrors bb_task_base_entry_t.sampled (bb_task.h) -- not itself
    // rendered, only used to gate free_bytes/used_bytes presence above.
    bool     sampled;
} bb_serialize_console_tasks_row_snap_t;

// Descriptor for bb_serialize_console_tasks_row_snap_t -- the SSOT
// bb_serialize_console_tasks_report() walks (once per row) via
// bb_serialize_console_render().
extern const bb_serialize_desc_t bb_serialize_console_tasks_row_desc;

// Snapshots every entry in bb_task's base registry (bb_task_base_foreach())
// into `dst` (a caller-owned array of `cap` bb_serialize_console_tasks_row_snap_t),
// copy-only (no I/O, matching bb_task_base_foreach()'s in-lock cb
// contract). Returns the number of rows written (0..cap) -- never more than
// `cap`, silently stopping there if the live registry holds more (the same
// truncate-don't-overflow contract as this header's render function). `dst`
// must be non-NULL when `cap` > 0.
size_t bb_serialize_console_tasks_gather(bb_serialize_console_tasks_row_snap_t *dst, size_t cap);

// Gathers a fresh task-stack snapshot and logs ONE bb_log_i() line PER TASK,
// each prefixed with `label` (e.g. "tick"), via
// bb_serialize_console_tasks_row_desc/bb_serialize_console_render(). No
// heap: the snapshot lives in a bounded file-static array (sized off
// BB_TASK_BASE_MAX_CAP, kept off this single caller's stack on purpose --
// see bb_serialize_console_tasks.c), plus a small on-stack per-line render
// buffer. A registry with zero tracked tasks logs nothing (not an error).
void bb_serialize_console_tasks_report(const char *label);

#ifdef __cplusplus
}
#endif
