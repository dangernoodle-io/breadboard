#pragma once

// bb_health_stack_wire — PUBLIC bb_serialize_desc_t (SSOT) for the
// "health.stack" bb_data key (B1-1045 PR-2, cutover composition-root
// ownership decision KB 1454). The composition root
// (examples/floor/main/floor_app.c, PR-4) owns wiring this descriptor to
// bb_data via bb_data_bind().
//
// Mirrors bb_health_stack_build_json()'s `{"task","free_bytes","low"}` shape
// (bb_health_stack.h) field-for-field. `free_bytes` is widened from
// `uint32_t` to a fixed int64_t -- bb_serialize_walk()'s BB_TYPE_I64 case
// always memcpy()s a fixed 8 bytes at the descriptor offset (see
// bb_serialize_walk.c); pointing a BB_TYPE_I64 field at a narrower uint32_t
// would read past it.

#include "bb_serialize.h"

#include "bb_core.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char    task[24];
    int64_t free_bytes;
    bool    low;
} bb_health_stack_wire_t;

_Static_assert(sizeof(((bb_health_stack_wire_t *)0)->free_bytes) == 8,
               "bb_health_stack_wire_t.free_bytes must be exactly 8 bytes for BB_TYPE_I64");

extern const bb_serialize_desc_t bb_health_stack_wire_desc;

#ifdef ESP_PLATFORM
// ESP-IDF only, not host-reproducible: copies the last-published
// health.stack state (s_last_stack in platform/espidf/bb_health/
// bb_health_stack.c, updated at both the low-stack transition and the
// initial low=false publish) into dst. Returns BB_ERR_INVALID_ARG if dst is
// NULL; otherwise BB_OK, even before either publish site has ever run
// (dst reads the struct's zero-initialized default: task="", free_bytes=0,
// low=false).
bb_err_t bb_health_stack_gather(bb_health_stack_wire_t *dst);
#endif
