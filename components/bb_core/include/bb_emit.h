#pragma once
#include <stddef.h>
#include <stdint.h>

// Generic bus-shaped emit callback: a primitive's publish edge in the
// (topic, id, payload, size) shape bb_event_emit expects, without
// pulling bb_event into the primitive.
//
// `ctx` (B1-1045 PR-1) is the FIRST parameter -- a caller-owned opaque
// pointer threaded through from the setter that registered this callback
// (see bb_callback_slot.h's BB_CALLBACK_SLOT_VOID_CTX) to the call site,
// letting N independent producers share one emit-shaped sink without a
// hidden static/pool. Existing bb_emit_fn consumers that don't need ctx
// pass/ignore NULL.
typedef void (*bb_emit_fn)(void *ctx, const char *topic, int32_t id,
                           const void *payload, size_t size);
