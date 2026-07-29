// Task-stack-over-serial periodic report: bb_task_base_foreach() (the SSOT
// per-task stack high-water free-bytes reader, populated by the periodic
// base-scan via bb_task_base_set_free_bytes() -- B1-1128's
// bb_num_words_to_bytes() SSOT, no second/third FreeRTOS sample here) ->
// one bb_serialize_console_render() + bb_log_i() call PER TASK (this
// backend is flat/scalar-only, see bb_serialize_console_emit()'s doc -- an
// array of task rows cannot render as one nested line). No periodic task of
// its own: a consumer calls bb_serialize_console_tasks_report() at its own
// cadence (mirrors bb_serialize_console_heap_report()'s milestone-driven
// contract).

#include "bb_serialize_console.h"

#include "bb_log.h"
#include "bb_str.h"
#include "bb_task.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_serialize_console_tasks";

// ---------------------------------------------------------------------------
// Kconfig bridge -- CONFIG_BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES -> a C
// default. Same symbol as bb_serialize_console_heap.c's own bridge (shared
// Kconfig option, independently bridged per file per this component's
// existing convention) -- never shadow the generated symbol with a bare
// #ifndef.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES
#define BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES CONFIG_BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES
#else
#define BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES 160
#endif

static bool row_budget_present(const void *snap)
{
    return ((const bb_serialize_console_tasks_row_snap_t *)snap)->stack_budget_bytes > 0;
}

// free_bytes is gated on `sampled` (bb_task_base_entry_t.sampled's mirror,
// bb_task.h), not merely present unconditionally: a task upserted since the
// last base-scan pass has free_bytes==0 with sampled==false, and rendering
// that 0 would misread as a genuine (alarming) low-stack reading rather
// than "not measured yet".
static bool row_free_bytes_present(const void *snap)
{
    return ((const bb_serialize_console_tasks_row_snap_t *)snap)->sampled;
}

// used_bytes is derived from free_bytes, so it inherits BOTH gates: a known
// budget (row_budget_present) AND an actual sample (row_free_bytes_present)
// -- a budget-known-but-unsampled task must not render used_bytes=0 either.
static bool row_used_bytes_present(const void *snap)
{
    return row_budget_present(snap) && row_free_bytes_present(snap);
}

static const bb_serialize_field_t s_tasks_row_fields[] = {
    { .key = "name", .type = BB_TYPE_STR,
      .offset = offsetof(bb_serialize_console_tasks_row_snap_t, name),
      .max_len = sizeof(((bb_serialize_console_tasks_row_snap_t *)0)->name) },
    { .key = "free_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_tasks_row_snap_t, free_bytes),
      .present = row_free_bytes_present },
    { .key = "budget_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_tasks_row_snap_t, stack_budget_bytes),
      .present = row_budget_present },
    { .key = "used_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_tasks_row_snap_t, used_bytes),
      .present = row_used_bytes_present },
};

const bb_serialize_desc_t bb_serialize_console_tasks_row_desc = {
    .type_name = "bb_serialize_console_tasks_row_snap_t",
    .fields = s_tasks_row_fields,
    .n_fields = sizeof(s_tasks_row_fields) / sizeof(s_tasks_row_fields[0]),
    .snap_size = sizeof(bb_serialize_console_tasks_row_snap_t),
};

// ---------------------------------------------------------------------------
// Gather -- copy-only snapshot under bb_task_base_foreach()'s lock (its
// contract, mirrored from bb_task_registry_foreach: I/O-free, bounded, no
// re-entry into bb_task_base_* from the callback).
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_console_tasks_row_snap_t *dst;
    size_t                                  cap;
    size_t                                  count;
} gather_ctx_t;

static void gather_cb(void *handle, const bb_task_base_entry_t *entry, void *vctx)
{
    (void)handle;
    gather_ctx_t *ctx = (gather_ctx_t *)vctx;
    if (ctx->count >= ctx->cap) {
        return;  // truncate rather than overflow dst -- see header contract
    }

    bb_serialize_console_tasks_row_snap_t *row = &ctx->dst[ctx->count];
    memset(row, 0, sizeof(*row));
    bb_strlcpy(row->name, entry->name, sizeof(row->name));
    row->free_bytes = (uint64_t)entry->free_bytes;
    row->sampled    = entry->sampled;
    if (entry->stack_bytes > 0) {
        row->stack_budget_bytes = (uint64_t)entry->stack_bytes;
        // Defensive clamp: free_bytes should never exceed a known budget,
        // but a task-registry unification edge case (budget learned/updated
        // after a stale free_bytes sample) is not provably impossible --
        // clamp to 0 rather than underflow the unsigned subtraction into a
        // huge, misleading "used" value. Written as an explicit if/else
        // (not a ternary) -- the bbtool `fence[sat_sub]` family freezes the
        // hand-rolled-saturating-subtract ternary/delta-clamp SHAPES as a
        // draining baseline (no canonical helper extracted yet, B3 parked);
        // this if/else is the same clamp, just not that shape.
        if (row->stack_budget_bytes > row->free_bytes) {
            row->used_bytes = row->stack_budget_bytes - row->free_bytes;
        } else {
            row->used_bytes = 0;
        }
    }
    ctx->count++;
}

size_t bb_serialize_console_tasks_gather(bb_serialize_console_tasks_row_snap_t *dst, size_t cap)
{
    if (!dst || cap == 0) return 0;

    gather_ctx_t ctx = { .dst = dst, .cap = cap, .count = 0 };
    bb_task_base_foreach(gather_cb, &ctx);
    return ctx.count;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

// File-static rather than a stack local: BB_TASK_BASE_MAX_CAP is an
// independent Kconfig (up to 64) and a stack array of that size would
// couple this report's caller's stack budget (e.g. the shared
// bb_timer_disp task, BB_TIMER_DISP_STACK) to it -- mirrors
// bb_task_registry.c's identical s_sw_wdt_snapshot rationale. Single-caller
// (bb_serialize_console_tasks_report has one runtime caller, a periodic
// timer job) so a shared static buffer is safe -- NOT reentrant.
static bb_serialize_console_tasks_row_snap_t s_task_rows[BB_TASK_BASE_MAX_CAP];

void bb_serialize_console_tasks_report(const char *label)
{
    const char *lbl = label ? label : "?";

    size_t n = bb_serialize_console_tasks_gather(s_task_rows, BB_TASK_BASE_MAX_CAP);

    char line[BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES];
    for (size_t i = 0; i < n; i++) {
        size_t out_len = 0;
        bb_err_t rrc = bb_serialize_console_render(&bb_serialize_console_tasks_row_desc, &s_task_rows[i],
                                                     line, sizeof(line), &out_len);
        // LCOV_EXCL_START -- unreachable: line/sizeof(line) are always valid (buf non-NULL, cap>0)
        if (rrc != BB_OK) {
            bb_log_e(TAG, "%s: task render failed (%d)", lbl, (int)rrc);
            continue;
        }
        // LCOV_EXCL_STOP

        bb_log_i(TAG, "%s task %s", lbl, line);
    }
}
