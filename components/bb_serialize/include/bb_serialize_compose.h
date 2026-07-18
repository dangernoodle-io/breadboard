#pragma once

/**
 * @brief Stateless composition driver over a caller-supplied array of
 * bb_serialize_desc_t/snapshot entries -- walks each entry against a shared
 * bb_serialize_emit_t in order, applying a per-entry wrapper (none, keyed
 * object, or unkeyed array element) chosen by the caller.
 *
 * No global state, no registry, no register/reset functions: each consumer
 * (e.g. a manifest or an OpenAPI doc assembler) owns its own entries[] array
 * however it likes -- bb_serialize_compose_walk() is a pure loop over
 * whatever it's handed.
 *
 * FLAT-STACK PROPERTY: at any instant at most ONE entry's bb_serialize_walk()
 * call is live on the C stack -- the driver is a loop, not nested recursion,
 * so peak stack usage is bounded by a SINGLE entry's descriptor depth, never
 * the sum over all entries. Each entry's begin_obj/end_obj wrapper pair (for
 * BB_SERIALIZE_COMPOSE_OBJECT/_ARRAY) costs one level of the emit backend's
 * own container DATA stack, but that pair fully closes before the next
 * entry's pair opens -- so composition never grows a backend's peak
 * container-nesting depth beyond one level over a single entry's own depth.
 * CAVEAT: an entry walked under OBJECT/ARRAY shape therefore runs one level
 * deeper than it would standalone -- a descriptor composed this way should
 * stay <= BB_SERIALIZE_MAX_DEPTH-1 internally, or it will bump into the
 * walker's own depth guard one level earlier than a bare bb_serialize_walk()
 * call against the same descriptor would.
 *
 * Does NOT emit an outer root container -- same convention as
 * bb_serialize_walk() itself: the caller owns root framing (e.g. a backend's
 * one-shot render entry point wraps the whole call in its own begin_obj/
 * end_obj if the wire format wants a root container).
 */

#include <stddef.h>

#include "bb_core.h"
#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

// Gather hook: fills `snap` (an entry's snapshot buffer, sized to its desc's
// snap_size) from live sources immediately before that entry is walked.
// Optional -- NULL means `snap` is already current and is walked as-is. The
// driver has no opinion on where a snap comes from: a gather may itself wrap
// e.g. bb_cache_serialize_get(), but any such staleness/generation-gating
// policy lives in that adapter, not here.
typedef bb_err_t (*bb_serialize_compose_gather_fn)(void *snap, void *ctx);

// One entry in a caller-owned composition array.
typedef struct {
    const char *name;                       // wire key for OBJECT shape; ignored for RAW/ARRAY
    const bb_serialize_desc_t *desc;        // borrowed (caller-owned)
    void *snap;                              // borrowed; sized to desc->snap_size
    bb_serialize_compose_gather_fn gather;  // optional; NULL = snap already current
    void *ctx;                               // passed to gather
} bb_serialize_compose_entry_t;

// Per-entry wrapper applied around each entry's walked fields.
typedef enum {
    // No per-entry wrapper: the entry's fields merge flatly into whatever
    // container the caller already has open around the whole
    // bb_serialize_compose_walk() call.
    BB_SERIALIZE_COMPOSE_RAW,
    // Per-entry: emit->begin_obj(ctx, entry.name) ... emit->end_obj(ctx).
    BB_SERIALIZE_COMPOSE_OBJECT,
    // Per-entry: emit->begin_obj(ctx, NULL) ... emit->end_obj(ctx) -- unkeyed,
    // as an array element (the caller is expected to have opened an
    // enclosing begin_arr/end_arr itself; this driver never calls those).
    BB_SERIALIZE_COMPOSE_ARRAY,
} bb_serialize_compose_shape_t;

// Walks entries[0..n) against `emit` in order, applying `shape`'s per-entry
// wrapper around each entry's bb_serialize_walk() call. ALL-OR-NOTHING: if
// any entry's gather (when non-NULL) returns a non-BB_OK code, or
// bb_serialize_walk() itself is never able to fail here (it is void), the
// walk aborts immediately at that entry -- no further entries are gathered
// or walked, and that error is returned verbatim. There is no partial-doc
// output beyond whatever the aborting entry's own emit calls already made
// before the abort was detected (a gather failure aborts BEFORE that entry's
// walk begins, so it contributes zero emit calls; a mid-walk failure isn't
// possible since bb_serialize_walk() itself cannot fail).
//
// Returns BB_ERR_INVALID_ARG if `emit` is NULL, if `entries` is NULL while
// `n` is nonzero, or if any entries[i].desc is NULL. n == 0 is valid and
// returns BB_OK having made zero emit calls.
bb_err_t bb_serialize_compose_walk(const bb_serialize_compose_entry_t *entries, size_t n,
                                    bb_serialize_compose_shape_t shape,
                                    const bb_serialize_emit_t *emit);

#ifdef __cplusplus
}
#endif
