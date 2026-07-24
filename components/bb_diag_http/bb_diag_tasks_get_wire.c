// bb_diag_tasks_get_wire — wire descriptor + fill for GET /api/diag/tasks.
// See bb_diag_tasks_get_wire_priv.h for the full shape/present-condition
// table this migration produces.

#include "bb_diag_tasks_get_wire_priv.h"

#include "bb_str.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Present predicates -- each reads only the row struct it's handed. Works
// identically inside a BB_ARR_STREAM row as at the top level:
// bb_serialize_walk()'s STREAM branch calls `f->present(row_buf)` with the
// same one-arg (const void *snap) contract as every other shape.
// ---------------------------------------------------------------------------

static bool row_core_present(const void *snap)
{
    return ((const bb_diag_tasks_get_wire_row_t *)snap)->core_present;
}

static bool row_runtime_present(const void *snap)
{
    return ((const bb_diag_tasks_get_wire_row_t *)snap)->runtime_present;
}

static bool row_registry_present(const void *snap)
{
    return ((const bb_diag_tasks_get_wire_row_t *)snap)->registry_present;
}

static bool row_sw_wdt_present(const void *snap)
{
    return ((const bb_diag_tasks_get_wire_row_t *)snap)->sw_wdt_present;
}

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

const bb_serialize_field_t bb_diag_tasks_get_wire_row_fields[13] = {
    { .key = "name", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, name),
      .max_len = sizeof(((bb_diag_tasks_get_wire_row_t *)0)->name) },
    { .key = "prio", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, prio) },
    { .key = "base_prio", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, base_prio) },
    { .key = "stack_hwm", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, stack_hwm) },
    { .key = "state", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, state) },
    { .key = "core", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, core),
      .present = row_core_present },
    { .key = "runtime", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, runtime),
      .present = row_runtime_present },
    { .key = "stack_budget_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, stack_budget_bytes),
      .present = row_registry_present },
    { .key = "wdt_subscribed", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, wdt_subscribed),
      .present = row_registry_present },
    { .key = "sw_wdt_timeout_ms", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, sw_wdt_timeout_ms),
      .present = row_sw_wdt_present },
    { .key = "sw_wdt_last_feed_age_ms", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, sw_wdt_last_feed_age_ms),
      .present = row_sw_wdt_present },
    { .key = "sw_wdt_miss_count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, sw_wdt_miss_count),
      .present = row_sw_wdt_present },
    { .key = "sw_wdt_last_miss_age_ms", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_tasks_get_wire_row_t, sw_wdt_last_miss_age_ms),
      .present = row_sw_wdt_present },
};

const uint16_t bb_diag_tasks_get_wire_row_n_fields =
    sizeof(bb_diag_tasks_get_wire_row_fields) / sizeof(bb_diag_tasks_get_wire_row_fields[0]);

// bb_diag_tasks_get_wire_row_n_fields (above) is a `const uint16_t`, not a
// constant expression -- it can't initialize `.n_children` even in this
// same TU. The literal below encodes the documented 13-field row shape
// above; a drift is caught by
// test_bb_diag_tasks_get_wire_row_field_count_matches asserting
// bb_diag_tasks_get_wire_row_n_fields == 13 against the SAME extern the
// runtime count comes from (mirrors bb_diag_sockets_get_wire.c's
// precedent).
#define BB_DIAG_TASKS_GET_ROW_N_FIELDS 13

static const bb_serialize_field_t s_diag_tasks_get_wire_registry_fields[3] = {
    { .key = "count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_tasks_get_wire_registry_t, count) },
    { .key = "capacity", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_tasks_get_wire_registry_t, capacity) },
    { .key = "dropped", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_tasks_get_wire_registry_t, dropped) },
};

static const bb_serialize_field_t s_diag_tasks_get_wire_fields[] = {
    { .key = "tasks", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_diag_tasks_get_wire_t, tasks),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_diag_tasks_get_wire_row_t),
      .cardinality = BB_ARR_STREAM,
      .children = bb_diag_tasks_get_wire_row_fields,
      .n_children = BB_DIAG_TASKS_GET_ROW_N_FIELDS },
    { .key = "registry", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_diag_tasks_get_wire_t, registry),
      .children = s_diag_tasks_get_wire_registry_fields,
      .n_children = sizeof(s_diag_tasks_get_wire_registry_fields) / sizeof(s_diag_tasks_get_wire_registry_fields[0]) },
};

const bb_serialize_desc_t bb_diag_tasks_get_wire_desc = {
    .type_name = "bb_diag_tasks_get_wire_t",
    .fields    = s_diag_tasks_get_wire_fields,
    .n_fields  = sizeof(s_diag_tasks_get_wire_fields) / sizeof(s_diag_tasks_get_wire_fields[0]),
    .snap_size = sizeof(bb_diag_tasks_get_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-3a meta-derivation feeder) --
// co-located JSON Schema companion to bb_diag_tasks_get_wire_desc above,
// gated behind BB_SERIALIZE_META_HOST (see bb_diag_tasks_get_wire_priv.h's
// banner). "required" mirrors platform/espidf/bb_diag_http/
// bb_diag_http_routes.c's hand-authored s_tasks_get_responses[] 200
// literal: "registry"'s 3 fields ARE required (unconditional); every
// per-task row field EXCEPT name/prio/base_prio/stack_hwm/state is
// present-gated at runtime (core/runtime/stack_budget_bytes/
// wdt_subscribed/sw_wdt_*), matching this table's non-required rows for
// those keys. See test_bb_diag_tasks_get_wire_meta_golden.c for the
// fidelity proof (including the documented ARR-of-OBJ nested-"required"
// engine gap, same precedent as bb_ota_validator_partitions_wire.c/
// bb_diag_sockets_get_wire.c).
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_diag_tasks_get_wire_row_meta_rows[] = {
    { .key = "name",       .required = true },
    { .key = "prio",       .required = true },
    { .key = "base_prio",  .required = true },
    { .key = "stack_hwm",  .required = true },
    { .key = "state",      .required = true },
    { .key = "core" },                     // present-gated (row_core_present) -- optional
    { .key = "runtime" },                  // present-gated (row_runtime_present) -- optional
    { .key = "stack_budget_bytes" },       // present-gated (row_registry_present) -- optional
    { .key = "wdt_subscribed" },           // present-gated (row_registry_present) -- optional
    { .key = "sw_wdt_timeout_ms" },        // present-gated (row_sw_wdt_present) -- optional
    { .key = "sw_wdt_last_feed_age_ms" },  // present-gated (row_sw_wdt_present) -- optional
    { .key = "sw_wdt_miss_count" },        // present-gated (row_sw_wdt_present) -- optional
    { .key = "sw_wdt_last_miss_age_ms" },  // present-gated (row_sw_wdt_present) -- optional
};

static const bb_serialize_field_meta_t s_diag_tasks_get_wire_registry_meta_rows[3] = {
    { .key = "count",    .required = true },
    { .key = "capacity", .required = true },
    { .key = "dropped",  .required = true },
};

static const bb_serialize_field_meta_t s_diag_tasks_get_wire_meta_rows[] = {
    { .key = "tasks", .required = true,
      .children = s_diag_tasks_get_wire_row_meta_rows,
      .n_children = sizeof(s_diag_tasks_get_wire_row_meta_rows) / sizeof(s_diag_tasks_get_wire_row_meta_rows[0]) },
    { .key = "registry", .required = true,
      .children = s_diag_tasks_get_wire_registry_meta_rows,
      .n_children = sizeof(s_diag_tasks_get_wire_registry_meta_rows) / sizeof(s_diag_tasks_get_wire_registry_meta_rows[0]) },
};

const bb_serialize_desc_meta_t bb_diag_tasks_get_wire_meta = {
    .type_name = "bb_diag_tasks_get_wire_t",
    .rows      = s_diag_tasks_get_wire_meta_rows,
    .n_rows    = sizeof(s_diag_tasks_get_wire_meta_rows) / sizeof(s_diag_tasks_get_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// Fill
// ---------------------------------------------------------------------------

void bb_diag_tasks_get_wire_fill_row(bb_diag_tasks_get_wire_row_t *row,
                                      const char *name, int64_t prio, int64_t base_prio,
                                      int64_t stack_hwm, const char *state_name,
                                      bool core_present, int64_t core,
                                      bool runtime_present, int64_t runtime,
                                      bool registry_present, uint64_t stack_budget_bytes,
                                      bool wdt_subscribed,
                                      bool sw_wdt_present, uint64_t sw_wdt_timeout_ms,
                                      uint64_t sw_wdt_last_feed_age_ms,
                                      uint64_t sw_wdt_miss_count,
                                      uint64_t sw_wdt_last_miss_age_ms)
{
    memset(row, 0, sizeof(*row));

    if (name) {
        bb_strlcpy(row->name, name, sizeof(row->name));
    }
    row->prio      = prio;
    row->base_prio = base_prio;
    row->stack_hwm = stack_hwm;

    const char *s = state_name ? state_name : "?";
    row->state = (bb_serialize_str_n_t){ .ptr = s, .len = strlen(s) };

    row->core_present = core_present;
    row->core         = core;

    row->runtime_present = runtime_present;
    row->runtime         = runtime;

    row->registry_present    = registry_present;
    row->stack_budget_bytes  = stack_budget_bytes;
    row->wdt_subscribed      = wdt_subscribed;

    row->sw_wdt_present            = sw_wdt_present;
    row->sw_wdt_timeout_ms         = sw_wdt_timeout_ms;
    row->sw_wdt_last_feed_age_ms   = sw_wdt_last_feed_age_ms;
    row->sw_wdt_miss_count         = sw_wdt_miss_count;
    row->sw_wdt_last_miss_age_ms   = sw_wdt_last_miss_age_ms;
}

void bb_diag_tasks_get_wire_fill_snap(bb_diag_tasks_get_wire_t *dst,
                                       const bb_diag_tasks_get_wire_row_t *rows, size_t n_rows,
                                       uint64_t registry_count, uint64_t registry_capacity,
                                       uint64_t registry_dropped)
{
    memset(dst, 0, sizeof(*dst));

    dst->registry.count    = registry_count;
    dst->registry.capacity = registry_capacity;
    dst->registry.dropped  = registry_dropped;

    dst->tasks = bb_serialize_arr_stream_from_buf(&dst->tasks_iter_state, rows, n_rows,
                                                    sizeof(bb_diag_tasks_get_wire_row_t));
}
