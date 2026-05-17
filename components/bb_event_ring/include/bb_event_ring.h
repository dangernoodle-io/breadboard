#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bb_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_event_ring *bb_event_ring_t;

// Attach a ring buffer of `capacity` entries to `topic`. Each entry up to `max_entry` bytes.
// Ring captures every post; oldest evicted when full.
bb_err_t bb_event_ring_attach(bb_event_topic_t topic, size_t capacity,
                              size_t max_entry, bb_event_ring_t *out);

// Subscribe with replay: ring synchronously replays buffered entries to `cb`,
// then registers cb on the topic for live events.
bb_err_t bb_event_ring_subscribe_with_replay(bb_event_ring_t ring,
                                             bb_event_handler_fn cb, void *user,
                                             bb_event_sub_t *out_sub);

// Stop capturing; free resources. `ring` is invalid after this.
void bb_event_ring_detach(bb_event_ring_t ring);

// Diagnostics: configured capacity of the ring.
size_t bb_event_ring_capacity(bb_event_ring_t ring);

// Diagnostics: number of entries currently held in the ring (0 → no replay possible).
size_t bb_event_ring_count(bb_event_ring_t ring);

// Diagnostics: metadata of the most recent entry captured.
// Populates *id, *size, *post_us (timestamp from esp_timer_get_time or clock_gettime).
// Returns BB_ERR_NOT_FOUND when the ring is empty.
// Any out-pointer may be NULL if the caller doesn't need that field.
bb_err_t bb_event_ring_last_entry_info(bb_event_ring_t ring,
                                       uint32_t *id,
                                       size_t *size,
                                       int64_t *post_us);

#ifdef __cplusplus
}
#endif
