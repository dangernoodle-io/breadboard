#pragma once

/**
 * @brief Compositional serialized-render cache (render memo), keyed
 * (format_id, key, state_version).
 *
 * bb_cache_serialize sits ABOVE bb_cache and bb_serialize as a pure
 * composition: it reads a key's canonical owned struct via
 * bb_cache_snapshot()/bb_cache_state_version() (the PR-3 API) and memoizes
 * the RENDERED wire bytes (e.g. JSON) for that key's CURRENT state_version,
 * so repeated gets against an unchanged value never re-render. A
 * bb_cache_update() bump to the key's state_version invalidates the memo on
 * the next get; no live eviction otherwise -- a fixed slot table, one memo
 * per (format, key) pair.
 *
 * Additive and composition-only: does NOT modify bb_cache's own
 * cached_json/dirty machinery (that is bb_cache's serializer-fn path, a
 * separate mechanism). This component is an independent consumer of
 * bb_cache's read-only snapshot API.
 */

#include "bb_core.h"
#include "bb_format.h"
#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns memoized (or freshly rendered + cached) wire bytes for (fmt,key) at
// the key's CURRENT bb_cache state_version. `desc` is caller-supplied (a
// static const bb_serialize_desc_t; a future PR may let the cache own it).
//
// `out` receives a pointer INTO the memo slot buffer (borrowed, read-only).
// This is a SINGLE-THREADED-CONSUMER contract: the pointer is safe to read
// ONLY while no other thread issues a bb_cache_serialize_get() for the SAME
// (fmt,key). A concurrent get on the same (fmt,key) that MISSES (its
// state_version bumped) re-renders into that same slot buffer under the
// internal lock and will TEAR bytes out from under a reader still consuming
// the old pointer -- the render is in-place, not copy-on-write. Callers MUST
// either serialize all gets for a given (fmt,key) onto one thread/queue, or
// copy the bytes out under their own lock before yielding to another thread.
//
// A get for a DIFFERENT (fmt,key) cannot invalidate this pointer, with ONE
// exception: a FAILED re-render of the SAME key marks its slot invalid and
// frees it for reuse, after which a subsequent get for a DIFFERENT (fmt,key)
// may claim that slot and overwrite the buffer. So the strict invariant is:
// no other get on the same key, AND no other get anywhere after a failed
// re-render of this key. This is why no multi-threaded consumer is wired up
// yet -- a hardened copy-out API is deferred (see the platform .c file's top
// comment for the deferred-work note).
//
// Returns BB_OK on a memo hit or a fresh render (bytes cached for next call).
// Returns BB_ERR_INVALID_ARG if key, desc, out, or out_len is NULL.
// Returns BB_ERR_NOT_FOUND if key is not registered in bb_cache.
// Returns BB_ERR_INVALID_STATE if key is registered in bb_cache GETTER mode
// (no owned struct to snapshot -- see bb_cache_snapshot()).
// Returns BB_ERR_NO_SPACE if the rendered bytes exceed the memo slot's fixed
// buffer capacity, or the slot table is full (no free slot for a new
// (fmt,key) pair).
// Returns BB_ERR_UNSUPPORTED if fmt has no wired rendering backend (any fmt
// other than BB_FORMAT_JSON, currently).
bb_err_t bb_cache_serialize_get(bb_format_t fmt, const char *key,
                                 const bb_serialize_desc_t *desc,
                                 const uint8_t **out, size_t *out_len);

#ifdef BB_CACHE_SERIALIZE_TESTING
// Test-only: reset the slot table to empty and zero the render counter.
void bb_cache_serialize_reset_for_test(void);

// Test-only: number of times bb_cache_serialize_get() has actually invoked a
// rendering backend (as opposed to serving a memo hit) since the last reset.
size_t bb_cache_serialize_render_count(void);
#endif

#ifdef __cplusplus
}
#endif
