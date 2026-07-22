#pragma once

// bb_display_info_wire — PUBLIC bb_serialize_desc_t (SSOT) for the
// "health.display" bb_cache topic (B1-1045 PR-2, cutover composition-root
// ownership decision KB 1454). ADDITIVE-only, INERT: no bb_data_bind() call
// exists anywhere in this PR -- the composition root (PR-4) is the sole
// owner of wiring this descriptor to bb_data.
//
// Mirrors bb_display_serialize()'s `{"present"[,"panel","width","height",
// "enabled"]}` shape (bb_display_info_event_priv.h) field-for-field.
// width/height are widened from `int` to a fixed int64_t -- bb_serialize_
// walk()'s BB_TYPE_I64 case always memcpy()s a fixed 8 bytes at the
// descriptor offset (see bb_serialize_walk.c); pointing a BB_TYPE_I64 field
// at a narrower `int` would read past it.

#include "bb_serialize.h"

#include "bb_core.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool    present;
    char    panel[32];
    int64_t width;
    int64_t height;
    bool    enabled;
} bb_display_info_wire_t;

_Static_assert(sizeof(((bb_display_info_wire_t *)0)->width) == 8,
               "bb_display_info_wire_t.width must be exactly 8 bytes for BB_TYPE_I64");
_Static_assert(sizeof(((bb_display_info_wire_t *)0)->height) == 8,
               "bb_display_info_wire_t.height must be exactly 8 bytes for BB_TYPE_I64");

extern const bb_serialize_desc_t bb_display_info_wire_desc;

// Portable (no ESP-IDF dep): reads the health.display bb_cache entry via
// bb_cache_get_raw() and widens width/height into `dst`. Returns
// BB_ERR_INVALID_ARG if dst is NULL; otherwise propagates bb_cache_get_raw()'s
// own return (BB_ERR_NOT_FOUND if the health.display key isn't registered
// yet).
bb_err_t bb_display_info_gather(bb_display_info_wire_t *dst);
