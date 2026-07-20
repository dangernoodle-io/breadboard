// bb_ring_diag -- see bb_ring_diag.h for the section contract.
// Pure/portable fill: bb_queue_registry_foreach() (B1-1077 PR-3a).
//
// Snapshot-first / copy-out pattern (MANDATORY per bb_queue_registry.h's
// foreach contract): bb_queue_registry_foreach holds its internal lock across
// the entire call, including every callback invocation -- this is what
// prevents a concurrent bb_queue_destroy() from freeing a ring mid-read
// (use-after-free). The callback here does only bounded memcpys/bb_strlcpy
// (no I/O, no allocation) while the lock is held; bb_diag_section_dispatch.c
// (the caller of this fill hook) does all the JSON streaming AFTER fill()
// returns and the lock is released.

#include "bb_ring_diag.h"

#include "bb_queue_registry.h"
#include "bb_str.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    bb_ring_diag_row_t *rows;
    int64_t              n;
    int64_t              max;
} rings_snapshot_ctx_t;

// Runs under bb_queue_registry's lock -- copy only, no I/O, no allocation.
static void rings_snapshot_cb(const char *name, bb_queue_t r, void *ctx)
{
    rings_snapshot_ctx_t *sc = (rings_snapshot_ctx_t *)ctx;
    if (sc->n >= sc->max) {
        return;  // LCOV_EXCL_LINE -- bb_queue_registry itself never holds more
                 // than BB_QUEUE_REGISTRY_MAX entries (bb_queue_create's
                 // registration is best-effort-capped at that same bound), so
                 // foreach can never invoke this callback past sc->max;
                 // defensive-only guard, unreachable via any real registry.
    }
    bb_ring_diag_row_t *row = &sc->rows[sc->n];
    // name is never NULL here: bb_queue_registry_foreach only ever invokes
    // this callback for entries stored via bb_queue_registry_register(),
    // which rejects a NULL name at registration time (bb_registry_register's
    // own !name check) -- so there is no live path that can hand this
    // callback a NULL name. No defensive ternary needed.
    bb_strlcpy(row->name, name, sizeof(row->name));
    // size_t -> int64_t widening is always safe here: these are diagnostic
    // ring counts/dropped/truncated (bounded by BB_QUEUE_REGISTRY_MAX-sized
    // rings) and cumulative byte totals -- none realistically approach
    // INT64_MAX on an embedded target.
    row->count      = (int64_t)bb_queue_count(r);
    row->capacity    = (int64_t)bb_queue_capacity(r);
    row->dropped     = (int64_t)bb_queue_dropped(r);
    row->truncated   = (int64_t)bb_queue_truncated(r);
    row->bytes_used  = (int64_t)bb_queue_bytes_used(r);
    sc->n++;
}

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_row_fields[] = {
    { .key = "name", .type = BB_TYPE_STR,
      .offset = offsetof(bb_ring_diag_row_t, name),
      .max_len = sizeof(((bb_ring_diag_row_t *)0)->name) },
    { .key = "count", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ring_diag_row_t, count) },
    { .key = "capacity", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ring_diag_row_t, capacity) },
    { .key = "dropped", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ring_diag_row_t, dropped) },
    { .key = "truncated", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ring_diag_row_t, truncated) },
    { .key = "bytes_used", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ring_diag_row_t, bytes_used) },
};

static const bb_serialize_field_t s_snap_fields[] = {
    { .key = "count", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ring_diag_snap_t, count) },
    { .key = "registry_capacity", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ring_diag_snap_t, registry_capacity) },
    { .key = "rings", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_ring_diag_snap_t, rings),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_ring_diag_row_t),
      .max_items = BB_QUEUE_REGISTRY_MAX,
      .children = s_row_fields,
      .n_children = sizeof(s_row_fields) / sizeof(s_row_fields[0]) },
};

const bb_serialize_desc_t bb_ring_diag_desc = {
    .type_name = "rings",
    .fields    = s_snap_fields,
    .n_fields  = sizeof(s_snap_fields) / sizeof(s_snap_fields[0]),
    .snap_size = sizeof(bb_ring_diag_snap_t),
};

// ---------------------------------------------------------------------------
// Fill
// ---------------------------------------------------------------------------

bb_err_t bb_ring_diag_fill(void *dst, const bb_diag_fill_args_t *args)
{
    (void)args;
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_ring_diag_snap_t *snap = (bb_ring_diag_snap_t *)dst;
    memset(snap, 0, sizeof(*snap));

    rings_snapshot_ctx_t sc = { .rows = snap->rings_items, .n = 0, .max = BB_QUEUE_REGISTRY_MAX };
    bb_queue_registry_foreach(rings_snapshot_cb, &sc);

    snap->count             = sc.n;
    snap->registry_capacity = BB_QUEUE_REGISTRY_MAX;
    snap->rings.items       = snap->rings_items;
    snap->rings.count       = (size_t)sc.n;

    return BB_OK;
}

#ifdef ESP_PLATFORM
bb_err_t bb_ring_diag_register(void)
{
    bb_diag_section_t section = {
        .name         = "rings",
        .desc         = "every live bb_queue_t registered via bb_queue_registry",
        .snap_desc    = &bb_ring_diag_desc,
        .fill         = bb_ring_diag_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    return bb_diag_register_section(&section);
}
#endif
