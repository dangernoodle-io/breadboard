#pragma once

// bb_partition_serialize -- a bb_serialize element descriptor for ONE
// bb_partition_info_t row, so a later consumer (the bb_diag `storage`
// section) can embed bb_partition_list()'s output as a nested ARR-of-OBJ
// field in its own snapshot descriptor. Additive-only: does not touch
// bb_partition_routes.c's existing hand-rolled /api/diag/partitions
// handler, which stays live until that route is retired.
//
// bb_partition_info_t's `offset`/`size` members are uint32_t, but
// bb_serialize_walk()'s BB_TYPE_U64 case always memcpy()s a fixed 8 bytes
// at the descriptor's field offset (see bb_serialize_walk.c) -- pointing a
// BB_TYPE_U64 field directly at a 4-byte member would read 4 bytes past it
// into whatever field follows (on this struct's layout, offset's overread
// would actually consume size's bytes too, silently combining the two into
// one wrong value). bb_meminfo_heap_snap_t hit the exact same fixed-width
// constraint and solved it by widening every numeric field to uint64_t in a
// dedicated snapshot struct; bb_partition_row_wire_t follows that same
// precedent for this row's two 32-bit numeric members. char[] and bool
// members are untouched (BB_TYPE_STR / BB_TYPE_BOOL already match their
// embedded width exactly), so bb_partition_row_wire_from_info() is a thin,
// pure, no-heap widening copy -- not a reshaping of the data.

#include "bb_partition.h"
#include "bb_serialize.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wire-shaped mirror of bb_partition_info_t, widened for
// bb_serialize_walk()'s fixed 8-byte BB_TYPE_U64 reads. Field order and
// char[] capacities match bb_partition_info_t exactly.
typedef struct {
    char     label[17];     // partition label (16 + NUL)
    char     type[8];       // "app" | "data" | "unknown"
    char     subtype[16];   // "factory","ota_0","ota_1","nvs","otadata","coredump","phy",...
    uint64_t offset;        // flash offset (address), widened from uint32_t
    uint64_t size;          // bytes, widened from uint32_t
    bool     running;       // == esp_ota_get_running_partition()
    bool     next_ota;      // == esp_ota_get_next_update_partition(NULL)
} bb_partition_row_wire_t;

// Widens one bb_partition_info_t row into its bb_partition_row_wire_t
// mirror. Pure, no heap, no I/O. NULL src or dst is a no-op (defensive; a
// descriptor-driven caller always supplies both).
void bb_partition_row_wire_from_info(bb_partition_row_wire_t *dst,
                                      const bb_partition_info_t *src);

// Element-level descriptor for ONE bb_partition_row_wire_t row -- a
// consumer embeds this as a BB_TYPE_ARR/elem_type=BB_TYPE_OBJ field's
// `children` in its own snapshot descriptor (e.g. the storage section's
// snapshot descriptor), with `elem_size = sizeof(bb_partition_row_wire_t)`
// and `items` pointing at a caller-owned bb_partition_row_wire_t[] filled
// via bb_partition_row_wire_from_info() per bb_partition_list() row.
extern const bb_serialize_field_t bb_partition_row_fields[];
extern const uint16_t             bb_partition_row_n_fields;

#ifdef __cplusplus
}
#endif
