// bb_diag_storage_partitions -- see bb_diag_storage_partitions.h for the
// section contract. Pure/portable fill: bb_partition_list() (already
// portable) widened per row via bb_partition_row_wire_from_info() (PR8).

#include "bb_diag_storage_partitions.h"

#include "bb_http.h"
#include "bb_partition.h"

#include <stddef.h>
#include <string.h>

// bb_partition_row_fields is declared `extern const bb_serialize_field_t
// bb_partition_row_fields[];` (an INCOMPLETE array type from this TU --
// bb_partition_serialize.h has no visibility into its own TU's array
// bound), so `.n_children` below cannot be `sizeof(...)/sizeof(...)`  the
// way bb_partition_serialize.c computes bb_partition_row_n_fields for
// itself. bb_partition_serialize.h documents the row descriptor as
// EXACTLY 7 fields ("a 7-field desc for bb_partition_info_t") -- the
// literal below encodes that documented invariant; a drift is caught by
// test_bb_diag_storage_partitions.c asserting
// bb_partition_row_n_fields == 7 against the SAME extern the runtime
// count comes from.
#define BB_PARTITION_ROW_N_FIELDS 7

static const bb_serialize_field_t s_snap_fields[] = {
    { .key = "rows", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_diag_storage_partitions_snap_t, rows),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_partition_row_wire_t),
      .max_items = BB_DIAG_STORAGE_PARTITIONS_ROW_CAP,
      .children = bb_partition_row_fields,
      .n_children = BB_PARTITION_ROW_N_FIELDS },
    { .key = "row_count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_storage_partitions_snap_t, row_count) },
};

const bb_serialize_desc_t bb_diag_storage_partitions_desc = {
    .type_name = "storage_partitions",
    .fields    = s_snap_fields,
    .n_fields  = sizeof(s_snap_fields) / sizeof(s_snap_fields[0]),
    .snap_size = sizeof(bb_diag_storage_partitions_snap_t),
};

// ---------------------------------------------------------------------------
// JSON Schema (B1-1180 PR-1) -- hand-authored, on-device (not host-gated;
// see bb_diag_storage_partitions.h's doc comment). Its byte-fidelity
// against the BB_SERIALIZE_META_HOST-gated co-located meta table below is
// proven by test/test_host/test_bb_diag_storage_partitions_meta_golden.c.
// ---------------------------------------------------------------------------

// A #define (not just the extern variable below) so the static-const
// describe route's response table (further down this file) can use the
// SAME literal text as a genuine compile-time constant expression --
// `.schema = bb_diag_storage_partitions_schema` (the VARIABLE's runtime
// value) is NOT a valid static/file-scope initializer in C ("initializer
// element is not constant"); `.schema =
// BB_DIAG_STORAGE_PARTITIONS_SCHEMA_LITERAL` (the macro-expanded string
// literal) is.
#define BB_DIAG_STORAGE_PARTITIONS_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"rows\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{" \
    "\"label\":{\"type\":\"string\"}," \
    "\"type\":{\"type\":\"string\"}," \
    "\"subtype\":{\"type\":\"string\"}," \
    "\"offset\":{\"type\":\"integer\"}," \
    "\"size\":{\"type\":\"integer\"}," \
    "\"running\":{\"type\":\"boolean\"}," \
    "\"next_ota\":{\"type\":\"boolean\"}}," \
    "\"additionalProperties\":false}}," \
    "\"row_count\":{\"type\":\"integer\"}}," \
    "\"required\":[\"rows\",\"row_count\"]," \
    "\"additionalProperties\":false}"

const char *const bb_diag_storage_partitions_schema = BB_DIAG_STORAGE_PARTITIONS_SCHEMA_LITERAL;

#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_diag_storage_partitions_row_meta_rows[] = {
    { .key = "label" },
    { .key = "type" },
    { .key = "subtype" },
    { .key = "offset" },
    { .key = "size" },
    { .key = "running" },
    { .key = "next_ota" },
};

static const bb_serialize_field_meta_t s_diag_storage_partitions_meta_rows[] = {
    { .key = "rows", .required = true,
      .children = s_diag_storage_partitions_row_meta_rows,
      .n_children = sizeof(s_diag_storage_partitions_row_meta_rows) / sizeof(s_diag_storage_partitions_row_meta_rows[0]) },
    { .key = "row_count", .required = true },
};

const bb_serialize_desc_meta_t bb_diag_storage_partitions_meta = {
    .type_name = "storage_partitions",
    .rows      = s_diag_storage_partitions_meta_rows,
    .n_rows    = sizeof(s_diag_storage_partitions_meta_rows) / sizeof(s_diag_storage_partitions_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

bb_err_t bb_diag_storage_partitions_fill(void *dst, const bb_diag_fill_args_t *args)
{
    (void)args;
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_diag_storage_partitions_snap_t *snap = (bb_diag_storage_partitions_snap_t *)dst;
    memset(snap, 0, sizeof(*snap));

    bb_partition_info_t info[BB_DIAG_STORAGE_PARTITIONS_ROW_CAP];
    size_t              total = 0;
    bb_err_t rc = bb_partition_list(info, BB_DIAG_STORAGE_PARTITIONS_ROW_CAP, &total);
    if (rc != BB_OK) return rc;

    size_t n = total < BB_DIAG_STORAGE_PARTITIONS_ROW_CAP ? total : BB_DIAG_STORAGE_PARTITIONS_ROW_CAP;
    for (size_t i = 0; i < n; i++) {
        bb_partition_row_wire_from_info(&snap->rows_items[i], &info[i]);
    }

    snap->rows.items = snap->rows_items;
    snap->rows.count = n;
    snap->row_count  = (uint64_t)total;

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Describe-only route (B1-1180 PR-1 review fix) -- a PRODUCER-OWNED
// `static const` bb_route_t (handler=NULL), .rodata/flash, never DRAM. See
// bb_diag_section_t.describe_route's doc comment
// (components/bb_diag/include/bb_diag_section.h) for the full mechanism.
// ---------------------------------------------------------------------------

static const bb_route_response_t s_diag_storage_partitions_describe_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_DIAG_STORAGE_PARTITIONS_SCHEMA_LITERAL },
    { .status = 0 },
};

static const bb_route_t s_diag_storage_partitions_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/storage/partitions",
    .tag       = "diag",
    .summary   = "Partition table inventory",
    .responses = s_diag_storage_partitions_describe_responses,
    .handler   = NULL,
};

#ifdef ESP_PLATFORM
bb_err_t bb_diag_storage_partitions_register(void)
{
    bb_diag_section_t section = {
        .name           = "storage/partitions",
        .desc           = "Partition table inventory",
        .snap_desc      = &bb_diag_storage_partitions_desc,
        .fill           = bb_diag_storage_partitions_fill,
        .ctx            = NULL,
        .query_keys     = NULL,
        .n_query_keys   = 0,
        .describe_route = &s_diag_storage_partitions_describe_route,
    };
    return bb_diag_register_section(&section);
}
#endif
