#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "bb_core.h"
#include "bb_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque port handle owned by the port impl.
typedef struct bb_event_port bb_event_port_t;

// Queue entry: fixed-stride allocation at init with payload inline.
// The port allocates fixed-stride slots of sizeof(bb_event_queue_entry_t) + max_payload bytes.
typedef struct {
    bb_event_topic_t topic;
    int32_t id;
    size_t size;
    // payload follows inline in allocated storage
} bb_event_queue_entry_t;

// Initialize the port with config knobs. Allocates queue + dispatcher resources.
bb_err_t bb_event_port_init(size_t queue_depth, size_t max_payload,
                            size_t stack_size, int task_priority);

// Enqueue a copy of (entry header + payload bytes). Non-blocking from any context.
bb_err_t bb_event_port_enqueue(const bb_event_queue_entry_t *hdr,
                               const void *payload);

// Cooperative drain (Arduino/host). Returns # dispatched.
// ESP-IDF impl can return 0 since dispatcher runs in dedicated task.
size_t   bb_event_port_drain(uint32_t budget);

// Mutex around the per-topic subscriber lists.
void     bb_event_port_lock(void);
void     bb_event_port_unlock(void);

// Called by port for each dequeued event; dispatches to subscribers under lock.
void     bb_event_common_dispatch(const bb_event_queue_entry_t *entry,
                                  const void *payload);

#ifdef __cplusplus
}
#endif
