// bb_attrs — intrusive membership header for filter/collection consumers.
//
// Any element a bb_filter selector operates over embeds a bb_attrs_t (as a
// named member, any position) and recovers its own type via
// bb_attrs_container_of() — the standard Linux-kernel offsetof idiom. This
// is metadata a filter reads; embedding it does not register the element
// anywhere or imply ownership.
//
// NO type_id field: no heterogeneous collection exists yet that needs
// runtime type recovery across mixed element kinds (YAGNI) — add one only
// when a real consumer needs it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Delivery class: MUST elements are never dropped by a filter under
// pressure (only deferred); DEFERRABLE elements may be shed under pressure.
#define BB_ATTRS_DELIVERY_MUST       0
#define BB_ATTRS_DELIVERY_DEFERRABLE 1

typedef struct {
    uint8_t  priority;         // lower value = more important
    uint16_t kind;             // caller-defined element kind (bitmask index; 0-15 usable against a uint16_t kind_mask)
    uint32_t tag_mask;         // caller-defined tag bits
    uint8_t  delivery_class;   // BB_ATTRS_DELIVERY_MUST or BB_ATTRS_DELIVERY_DEFERRABLE
} bb_attrs_t;

// Recover the owning struct from a pointer to its embedded bb_attrs_t
// member. Standard Linux container_of/offsetof idiom.
#define bb_attrs_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#ifdef __cplusplus
}
#endif
