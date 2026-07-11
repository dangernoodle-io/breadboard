#pragma once
#include <stddef.h>
#include <stdint.h>

// Generic bus-shaped emit callback: a primitive's publish edge in the
// (topic, id, payload, size) shape bb_event_emit expects, without
// pulling bb_event into the primitive.
typedef void (*bb_emit_fn)(const char *topic, int32_t id,
                           const void *payload, size_t size);
