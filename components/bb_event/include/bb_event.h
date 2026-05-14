#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_event_topic *bb_event_topic_t;
typedef struct bb_event_sub   *bb_event_sub_t;

typedef void (*bb_event_handler_fn)(bb_event_topic_t topic,
                                    int32_t id,
                                    const void *data, size_t size,
                                    void *user);

typedef struct {
    size_t queue_depth;   // 0 -> default 16
    size_t max_payload;   // 0 -> default 256 (per-post cap)
    size_t stack_size;    // ESP-IDF dispatcher task; ignored elsewhere
    int    task_priority; // ESP-IDF; ignored elsewhere
} bb_event_cfg_t;

// Initialize the event bus with optional config; idempotent, second call no-ops.
bb_err_t bb_event_init(const bb_event_cfg_t *cfg);

// Register a topic by name; returns same handle for duplicate names.
bb_err_t bb_event_topic_register(const char *name, bb_event_topic_t *out);

// Look up a topic by name; returns BB_ERR_NOT_FOUND if absent.
bb_err_t bb_event_topic_lookup(const char *name, bb_event_topic_t *out);

// Subscribe to a topic; handler fires on every post to that topic.
bb_err_t bb_event_subscribe(bb_event_topic_t topic,
                            bb_event_handler_fn cb, void *user,
                            bb_event_sub_t *out_sub);

// Unsubscribe and return the handle to the free list.
bb_err_t bb_event_unsubscribe(bb_event_sub_t sub);

// Post an event to a topic; payload is copied into the queue.
bb_err_t bb_event_post(bb_event_topic_t topic, int32_t id,
                       const void *data, size_t size);

// Cooperative dispatch: drain up to budget events (or all if budget=0); returns count dispatched.
size_t   bb_event_pump(uint32_t budget);

// Serialization primitive for consumers that maintain state mutated both from
// subscriber callbacks AND from outside dispatch (e.g. bb_event_ring). Held
// across snapshot/replay setup in bb_event_subscribe_with_prep.
void     bb_event_lock(void);
void     bb_event_unlock(void);

// Run prep(prep_arg) under bb_event's lock, then atomically subscribe so no
// events dispatch on `topic` between prep returning and the subscription
// becoming active. prep may be NULL.
bb_err_t bb_event_subscribe_with_prep(bb_event_topic_t topic,
                                      bb_event_handler_fn cb, void *user,
                                      void (*prep)(void *prep_arg),
                                      void *prep_arg,
                                      bb_event_sub_t *out_sub);

#ifdef __cplusplus
}
#endif
