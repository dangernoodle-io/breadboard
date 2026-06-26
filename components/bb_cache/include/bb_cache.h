#pragma once

// bb_cache — canonical state-topic cache + shared serializer.
//
// ONE registered serializer per topic guarantees that SSE event payloads and
// REST handler payloads are identical by construction: both call the same
// bb_cache_serialize_fn against the same canonical struct.
//
// Ownership model:
//   snapshot == NULL  — bb_cache owns the struct; caller copies in via
//                       bb_cache_update(); snap_size = sizeof(struct).
//   snapshot != NULL  — topic owns the struct; bb_cache reads through the
//                       getter; snap_size is ignored; bb_cache_update is no-op.

#include "bb_core.h"
#include "bb_json.h"

// ---------------------------------------------------------------------------
// Capacity constant (Kconfig bridge — pattern from bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_CACHE_MAX_TOPICS
#define BB_CACHE_MAX_TOPICS CONFIG_BB_CACHE_MAX_TOPICS
#endif
#endif
#ifndef BB_CACHE_MAX_TOPICS
#define BB_CACHE_MAX_TOPICS 8
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Serializer fn: writes fields from *snap into obj.
// Called under the per-entry lock with a valid snap pointer.
typedef void (*bb_cache_serialize_fn)(bb_json_t obj, const void *snap);

// Register a state topic.
//   topic      — topic name string (e.g. "net.health"); must outlive the registry.
//   snapshot   — nullable getter fn: returns a pointer to the canonical struct.
//                Pass NULL to have bb_cache own the struct (cop in via update).
//   snap_size  — sizeof the owned struct; ignored when snapshot != NULL.
//   serialize  — serializer fn invoked by both post and serialize_into.
bb_err_t bb_cache_register(const char *topic,
                           const void *(*snapshot)(void),
                           size_t snap_size,
                           bb_cache_serialize_fn serialize);

// Copy *snap into the owned struct under the per-entry lock.
// No-op (returns BB_OK) when the topic was registered with a getter.
// Returns BB_ERR_NOT_FOUND if the topic is not registered.
bb_err_t bb_cache_update(const char *topic, const void *snap);

// Serialize the cached struct into a fresh bb_json obj and post it via
// bb_event_post to the registered event topic. Does NOT touch any ring —
// ring attachment is the caller's responsibility.
// Returns BB_ERR_NOT_FOUND if the topic is not registered.
bb_err_t bb_cache_post(const char *topic);

// Serialize the cached struct into a caller-supplied bb_json obj.
// Identical output to what bb_cache_post would emit — this is the REST path.
// Returns BB_ERR_NOT_FOUND if the topic is not registered.
bb_err_t bb_cache_serialize_into(const char *topic, bb_json_t obj);

#ifdef __cplusplus
}
#endif
