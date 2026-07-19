// bb_partition_serialize -- the bb_serialize field-table SSOT for one
// bb_partition_info_t row. Pure .rodata + one thin widening helper; compiles
// on both host and ESP-IDF (bb_partition_info_t and bb_serialize.h are both
// already portable). See bb_partition_serialize.h for the widening rationale.

#include "bb_partition_serialize.h"

#include <stddef.h>
#include <string.h>

void bb_partition_row_wire_from_info(bb_partition_row_wire_t *dst,
                                      const bb_partition_info_t *src)
{
    if (!dst || !src) return;

    memcpy(dst->label, src->label, sizeof(dst->label));
    memcpy(dst->type, src->type, sizeof(dst->type));
    memcpy(dst->subtype, src->subtype, sizeof(dst->subtype));
    dst->offset = (uint64_t)src->offset;
    dst->size = (uint64_t)src->size;
    dst->running = src->running;
    dst->next_ota = src->next_ota;
}

const bb_serialize_field_t bb_partition_row_fields[] = {
    { .key = "label", .type = BB_TYPE_STR,
      .offset = offsetof(bb_partition_row_wire_t, label), .max_len = 17 },
    { .key = "type", .type = BB_TYPE_STR,
      .offset = offsetof(bb_partition_row_wire_t, type), .max_len = 8 },
    { .key = "subtype", .type = BB_TYPE_STR,
      .offset = offsetof(bb_partition_row_wire_t, subtype), .max_len = 16 },
    { .key = "offset", .type = BB_TYPE_U64,
      .offset = offsetof(bb_partition_row_wire_t, offset) },
    { .key = "size", .type = BB_TYPE_U64,
      .offset = offsetof(bb_partition_row_wire_t, size) },
    { .key = "running", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_partition_row_wire_t, running) },
    { .key = "next_ota", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_partition_row_wire_t, next_ota) },
};

const uint16_t bb_partition_row_n_fields =
    sizeof(bb_partition_row_fields) / sizeof(bb_partition_row_fields[0]);
