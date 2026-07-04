#pragma once

// bb_cache — canonical state-key cache + shared serializer.
//
// ONE registered serializer per key guarantees that SSE event payloads and
// REST handler payloads are identical by construction: both call the same
// bb_cache_serialize_fn against the same canonical struct.
//
// Ownership model:
//   snapshot == NULL  — bb_cache owns the struct; caller copies in via
//                       bb_cache_update(); snap_size = sizeof(struct).
//   snapshot != NULL  — key owns the struct; bb_cache reads through the
//                       getter; snap_size is ignored; bb_cache_update is no-op.

#include "bb_core.h"
#include "bb_json.h"

// ---------------------------------------------------------------------------
// Capacity constants (Kconfig bridge — pattern from bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_CACHE_MAX_TOPICS
#define BB_CACHE_MAX_TOPICS CONFIG_BB_CACHE_MAX_TOPICS
#endif
#ifdef CONFIG_BB_CACHE_KEY_MAX
#define BB_CACHE_KEY_MAX CONFIG_BB_CACHE_KEY_MAX
#endif
#endif
#ifndef BB_CACHE_MAX_TOPICS
#define BB_CACHE_MAX_TOPICS 32
#endif
#ifndef BB_CACHE_KEY_MAX
#define BB_CACHE_KEY_MAX 96
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability flags
// ---------------------------------------------------------------------------

/** Flags passed to bb_cache_config_t.flags to control per-key behaviour. */
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

// Configuration for a registered cache entry.
//
//   key        — registry identity string (e.g. "net.health"). COPIED into
//                the registry (up to BB_CACHE_KEY_MAX-1 chars, NUL-
//                terminated) — the caller does NOT need to keep it alive
//                past the bb_cache_register() call. Becomes the wire SSE
//                event topic only when flags has BB_CACHE_FLAG_SSE.
//   snapshot   — nullable getter fn: returns a pointer to the canonical
//                struct. Pass NULL to have bb_cache own the struct (copy in
//                via bb_cache_update()).
//   snap_size  — sizeof the owned struct; required when snapshot == NULL,
//                ignored when snapshot != NULL.
//   serialize  — serializer fn invoked by both post and serialize_into.
//   flags      — BB_CACHE_FLAG_* bitmask. Use BB_CACHE_FLAG_SSE to also
//                register an event topic for SSE fan-out via bb_cache_post.
//                Use BB_CACHE_FLAG_NONE for sink-only entries that do not
//                need SSE delivery. Zero-initializing flags is equivalent
//                to BB_CACHE_FLAG_NONE — callers that need SSE MUST set
//                this explicitly.
typedef struct {
    const char             *key;
    const void            *(*snapshot)(void);
    size_t                  snap_size;
    bb_cache_serialize_fn   serialize;
    bb_cache_flags_t        flags;
} bb_cache_config_t;

// Register a cache entry.
//
// Returns BB_ERR_INVALID_ARG if cfg, cfg->key, or cfg->serialize is NULL, or
// if strlen(cfg->key) >= BB_CACHE_KEY_MAX (over-length keys are rejected
// loudly at register time, never silently truncated).
// Returns BB_ERR_NO_SPACE if the registry is full, or (owned mode) the
// snapshot buffer could not be allocated.
// Idempotent: registering an already-registered key returns BB_OK without
// creating a duplicate entry.
bb_err_t bb_cache_register(const bb_cache_config_t *cfg);

// Copy *snap into the owned struct under the per-entry lock.
// No-op (returns BB_OK) when the key was registered with a getter.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
bb_err_t bb_cache_update(const char *key, const void *snap);

// Serialize the cached struct into a fresh bb_json obj and post it via
// bb_event_post to the registered event topic. Does NOT touch any ring —
// ring attachment is the caller's responsibility.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
// Returns BB_ERR_INVALID_STATE if the key has no SSE event topic
// (registered with BB_CACHE_FLAG_NONE).
bb_err_t bb_cache_post(const char *key);

// Serialize the cached struct into a caller-supplied bb_json obj.
// Identical output to what bb_cache_post would emit — this is the REST path.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
bb_err_t bb_cache_serialize_into(const char *key, bb_json_t obj);

// Post a pre-serialized payload to the key's SSE event channel.
// Use this when the caller has ALREADY serialized the snapshot (e.g. the
// bb_pub sampler serializes once for both SSE and sinks).  Avoids the
// extra serialize call that bb_cache_post would perform.
// Returns BB_ERR_INVALID_STATE when the key has no SSE event topic.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
bb_err_t bb_cache_post_serialized(const char *key, const char *json, size_t json_len);

// Memoized serialization — the core of serialize-once, COPY-OUT under the lock.
//
// Copies the key's last serialized JSON (NUL-terminated) into the caller's
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
// For getter-mode entries (registered with a non-NULL snapshot getter), the
// cache has no dirty signal, so the serializer runs on every call (the data
// may change underneath without an update). Owned-mode entries (snapshot==NULL)
// get true memoization via the dirty flag.
//
// Use bb_cache_serialize_into instead when EMBEDDING a key as a section in a
// larger composed document (e.g. /api/health aggregating multiple keys).
//
//   key      — registered key.
//   buf      — caller-owned destination buffer (must be non-NULL).
//   cap      — capacity of buf in bytes (must include room for the NUL).
//   out_len  — optional; receives strlen of the copied JSON (excludes NUL).
//
// Returns BB_ERR_NOT_FOUND if the key is not registered.
// Returns BB_ERR_INVALID_STATE if no snapshot is available yet.
// Returns BB_ERR_NO_SPACE on serialize allocation failure OR if cap is too
// small to hold the serialized JSON plus its NUL terminator (buf untouched).
bb_err_t bb_cache_get_serialized(const char *key, char *buf, size_t cap, size_t *out_len);

// ---------------------------------------------------------------------------
// Keyed enumeration + compact struct-read accessor
// ---------------------------------------------------------------------------

// Number of currently registered (non-NULL key) entries in the registry.
size_t bb_cache_count(void);

// Look up the key at a raw registry slot index.
//   index   — raw slot index, [0, BB_CACHE_MAX_TOPICS).
//   out_key — receives s_entries[index].key; may be NULL for a free slot.
// Returns BB_ERR_INVALID_ARG if out_key is NULL.
// Returns BB_ERR_NOT_FOUND if index >= BB_CACHE_MAX_TOPICS.
bb_err_t bb_cache_key_at(size_t index, const char **out_key);

// Invoke cb once per registered key. The key set is snapshotted under the
// registry lock and cb is invoked with the lock released, so cb may safely
// call bb_cache_* (lock not held during cb).
bb_err_t bb_cache_foreach(void (*cb)(const char *key, void *ctx), void *ctx);

// Compact read of an owned-mode key's raw struct bytes.
//   buf — caller-owned destination buffer (must be non-NULL).
//   cap — capacity of buf in bytes (must be non-zero).
// Copies the full owned struct into buf; refuses and does NOT copy if
// cap < key's registered size. Parity with bb_cache_get_serialized:
// refuses rather than truncates on undersized buffer.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
// Returns BB_ERR_INVALID_STATE for getter-mode keys (no owned struct).
// Returns BB_ERR_INVALID_ARG on null args or cap == 0.
// Returns BB_ERR_NO_SPACE if cap < the stored struct size (buf untouched).
bb_err_t bb_cache_get_raw(const char *key, void *buf, size_t cap);

#ifdef __cplusplus
}
#endif
