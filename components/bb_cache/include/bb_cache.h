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
#define BB_CACHE_MAX_TOPICS 32
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability flags
// ---------------------------------------------------------------------------

/** Flags passed to bb_cache_register_ex to control per-topic behaviour. */
typedef uint32_t bb_cache_flags_t;

/** No special flags — owned struct, no SSE event topic registered. */
#define BB_CACHE_FLAG_NONE  (0u)
/** Register an SSE event topic so bb_cache_post can fan out to SSE clients. */
#define BB_CACHE_FLAG_SSE   (1u << 0)

// ---------------------------------------------------------------------------
// Serializer type
// ---------------------------------------------------------------------------

// Serializer fn: writes fields from *snap into obj.
// Called under the per-entry lock with a valid snap pointer.
typedef void (*bb_cache_serialize_fn)(bb_json_t obj, const void *snap);

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// Register a state topic (extended form).
//   topic      — topic name string (e.g. "net.health"); must outlive the registry.
//   snapshot   — nullable getter fn: returns a pointer to the canonical struct.
//                Pass NULL to have bb_cache own the struct (copy in via update).
//   snap_size  — sizeof the owned struct; ignored when snapshot != NULL.
//   serialize  — serializer fn invoked by both post and serialize_into.
//   flags      — BB_CACHE_FLAG_* bitmask. Use BB_CACHE_FLAG_SSE to also
//                register an event topic for SSE fan-out via bb_cache_post.
//                Use BB_CACHE_FLAG_NONE for sink-only topics that do not
//                need SSE delivery.
bb_err_t bb_cache_register_ex(const char *topic,
                               const void *(*snapshot)(void),
                               size_t snap_size,
                               bb_cache_serialize_fn serialize,
                               bb_cache_flags_t flags);

// Register a state topic (legacy form — equivalent to bb_cache_register_ex
// with BB_CACHE_FLAG_SSE; zero behaviour change for all existing callers).
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
// Returns BB_ERR_INVALID_STATE if the topic has no SSE event topic
// (registered with BB_CACHE_FLAG_NONE).
bb_err_t bb_cache_post(const char *topic);

// Serialize the cached struct into a caller-supplied bb_json obj.
// Identical output to what bb_cache_post would emit — this is the REST path.
// Returns BB_ERR_NOT_FOUND if the topic is not registered.
bb_err_t bb_cache_serialize_into(const char *topic, bb_json_t obj);

// Post a pre-serialized payload to the topic's SSE event channel.
// Use this when the caller has ALREADY serialized the snapshot (e.g. the
// bb_pub sampler serializes once for both SSE and sinks).  Avoids the
// extra serialize call that bb_cache_post would perform.
// Returns BB_ERR_INVALID_STATE when the topic has no SSE event topic.
// Returns BB_ERR_NOT_FOUND if the topic is not registered.
bb_err_t bb_cache_post_serialized(const char *topic, const char *json, size_t json_len);

// Memoized serialization — the core of serialize-once, COPY-OUT under the lock.
//
// Copies the topic's last serialized JSON (NUL-terminated) into the caller's
// buffer. The serializer runs AT MOST ONCE per bb_cache_update() generation:
// the first reader after an update serializes and caches the bytes; subsequent
// readers (SSE post, every sink, REST polls) get a COPY of the same cached
// string without re-serializing.
//
// UAF-safe by construction: the copy happens under the entry lock, and the
// caller only ever touches its own buffer. A concurrent bb_cache_update() +
// re-serialize (which frees the entry's internal buffer) can never corrupt an
// in-flight reader, because no caller holds the cache-owned pointer past the
// lock. Size the buffer to the cache's max payload (e.g. the bb_pub worker
// uses CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES); REST handlers use a comparable
// stack/heap buffer.
//
// For getter-mode topics (registered with a non-NULL snapshot getter), the
// cache has no dirty signal, so the serializer runs on every call (the data
// may change underneath without an update). Owned-mode topics (snapshot==NULL)
// get true memoization via the dirty flag.
//
// Use bb_cache_serialize_into instead when EMBEDDING a topic as a section in a
// larger composed document (e.g. /api/health aggregating multiple topics).
//
//   topic    — registered topic name.
//   buf      — caller-owned destination buffer (must be non-NULL).
//   cap      — capacity of buf in bytes (must include room for the NUL).
//   out_len  — optional; receives strlen of the copied JSON (excludes NUL).
//
// Returns BB_ERR_NOT_FOUND if the topic is not registered.
// Returns BB_ERR_INVALID_STATE if no snapshot is available yet.
// Returns BB_ERR_NO_SPACE on serialize allocation failure OR if cap is too
// small to hold the serialized JSON plus its NUL terminator (buf untouched).
bb_err_t bb_cache_get_serialized(const char *topic, char *buf, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
