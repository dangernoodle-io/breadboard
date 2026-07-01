#pragma once

// bb_event_topic_registry — private register-only index over the attached-
// topics table backing bb_event_routes_attach_ex2/capture_cb/topic_count/
// topic_info. Two implementations exist behind this portable, pthread-free
// header:
//
//   - platform/host/bb_event_routes/bb_event_topic_registry.c (host +
//     ESP-IDF): wraps the generic bb_registry primitive (which hard-includes
//     <pthread.h> — POSIX/ESP-IDF only).
//   - platform/arduino/bb_event_routes/bb_event_routes_arduino.cpp: a plain
//     linear-array scan, no lock (Arduino's loop() is single-threaded).
//
// This header stays free of <pthread.h> and "bb_registry.h" so that
// bb_event_routes_common.c — which is compiled on Arduino/AVR too (CC3000) —
// never pulls in POSIX pthreads.
//
// Register-only: there is no deregister. Attached topics persist for the
// life of the process (bb_event_routes_attach* never detaches). Index
// stability (the idx returned by bb_event_topic_registry_find_by_handle, and
// the ordering walked by bb_event_topic_registry_get_by_index) depends on
// this no-deregister invariant — the underlying generic bb_registry
// primitive compacts on removal, so if a deregister path is ever added here,
// this stability contract must be revisited.

#include <stddef.h>
#include "bb_core.h"
#include "bb_event.h"
#include "bb_event_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BB_EVENT_TOPIC_NAME_MAX 32

typedef struct {
    char name[BB_EVENT_TOPIC_NAME_MAX];
    bb_event_topic_t topic;
    bb_event_ring_t ring;
} bb_event_attached_topic_t;

// Register `t` (already fully populated) under `name`. `name` MUST be a
// pointer with process lifetime — callers pass `t->name` (the persistent
// copy baked into the entry itself), never the caller-supplied transient
// topic-name argument.
// Returns BB_OK on success OR on a duplicate name (idempotent — mirrors
// bb_event_routes_attach's idempotent-per-topic contract).
// Returns BB_ERR_NO_SPACE if the table is full.
// Returns BB_ERR_INVALID_ARG if name or t is NULL.
bb_err_t bb_event_topic_registry_register(const char *name, bb_event_attached_topic_t *t);

// Find the attached-topic entry whose ->topic field matches `topic`.
// Returns BB_OK and sets *out_idx on a hit; BB_ERR_NOT_FOUND on a miss.
// Returns BB_ERR_INVALID_ARG if topic or out_idx is NULL.
bb_err_t bb_event_topic_registry_find_by_handle(bb_event_topic_t topic, size_t *out_idx);

// Current registered count.
size_t bb_event_topic_registry_count(void);

// Copy the pointer stored at index idx (registration order — stable under
// the no-deregister invariant documented above).
// Returns BB_ERR_INVALID_ARG if out is NULL.
// Returns BB_ERR_NOT_FOUND if idx >= count.
bb_err_t bb_event_topic_registry_get_by_index(size_t idx, bb_event_attached_topic_t **out);

#ifdef BB_EVENT_ROUTES_TESTING
// Reset to empty. Test teardown only.
void bb_event_topic_registry_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
