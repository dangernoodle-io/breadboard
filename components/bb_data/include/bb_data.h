#pragma once

/**
 * @brief bb_data core binding table (B1-832) -- OWNS the `key -> (desc,
 * gather)` binding table for the future bidirectional data path (the
 * B1-828 epic replacing bb_pub + bb_sub + all bb_sink_*). DIRECT: bb_data
 * delegates ONLY the wire-format step to the existing bb_serialize
 * format-dispatch registry (bb_serialize_format.h) -- it does NOT wrap
 * bb_cache or bb_cache_serialize, and has no dependency on either.
 *
 * EGRESS ONLY for now. Ingress (populate/scatter inbound wire bytes into a
 * bound destination) is deferred: it needs a resolved format-registry
 * parse-contract design (bb_serialize's per-format `.parse` slot shape
 * varies by backend -- e.g. JSON's registered `.parse` is a token-scan
 * handle, not a one-shot deserialize function -- so there is no single
 * function-pointer shape a generic bb_data populate path could safely cast
 * through today). That design fork is not resolved here; this PR ships the
 * egress (render) half only.
 *
 * A consumer calls bb_data_bind() once per key (typically from the
 * composition root) to register a snapshot descriptor plus a gather hook
 * (fills a scratch snapshot on demand). bb_data_render() then drives that
 * binding through whichever wire format the caller names, entirely against
 * CALLER-OWNED buffers -- bb_data holds no static scratch of its own
 * (reentrant, no lock needed on the render path).
 *
 * bb_data_bind() itself is a composition-time-only call: it MUST be invoked
 * only during single-threaded composition, before any bb_data_render()
 * traffic starts. Rebinding an existing key after render traffic has begun
 * is unsupported -- this matches the established lock-free, composition-time
 * bb_registry model used across breadboard (no new freeze/lock primitive
 * here).
 *
 * bb_data_get() (a future memoized read path) is deliberately NOT part of
 * this PR's surface.
 */

#include "bb_core.h"
#include "bb_format.h"
#include "bb_serialize.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed binding-table capacity -- a handful of composed keys, not an
// open-ended runtime set (mirrors bb_serialize_format's own small registry
// sizing rationale). No Kconfig bridge -- this is a compile-time-only
// constant, not a per-target tunable. The B1-1045 cutover briefly bumped
// this 8->16 in anticipation of examples/floor binding 8 keys; floor's
// scope narrowed to 3 (log, diag.meminfo, diag.system), so 8 is ample
// headroom again.
#define BB_DATA_MAX_BINDINGS 8

// Max length (including the terminating NUL) of a binding key -- a short
// composed identifier, not user-controlled wire data. bb_data_bind()
// rejects (BB_ERR_INVALID_ARG) any key whose strlen() is >= this bound
// rather than silently truncating it.
#define BB_DATA_KEY_MAX 48

// Egress hook: fills `dst` (the binding's descriptor's snap_size bytes,
// CALLER-OWNED scratch storage sized by the bb_data_render() caller) from
// live sources.
typedef bb_err_t (*bb_data_gather_fn)(void *dst, void *ctx);

// Replay semantics for a binding's future broadcast path (B1-1033) -- STORED
// only here, not yet consumed by anything in this PR.
//
// BB_DATA_STATE (the default -- see bb_data_binding_t.replay_kind below): a
// fresh render on connect, no backlog ring. Suits a binding whose current
// value fully describes it (e.g. a heap snapshot) -- a late subscriber just
// wants "now", not history.
//
// BB_DATA_EVENT: a bounded ring of past values, flushed to a subscriber on
// connect. Suits a binding whose individual values are discrete occurrences
// a late subscriber still needs (e.g. a log/alert stream) rather than a
// single current state.
typedef enum {
    BB_DATA_STATE = 0,
    BB_DATA_EVENT = 1,
} bb_data_replay_kind_t;

// One key's binding. `desc` is BORROWED (typically a static const owned by
// the source component, e.g. bb_meminfo_heap_snap_desc) -- the caller keeps
// it alive for the life of the binding; bb_data never copies or frees it.
// `gather`/`ctx` are the egress path; `gather` MUST be set (a binding with
// no gather is useless -- see bb_data_bind()). `replay_kind` defaults to
// BB_DATA_STATE when zero-initialized (e.g. via a partial struct literal
// that omits the field) -- every existing call site keeps today's
// fresh-render-only behavior with no change required.
typedef struct {
    const char                *key;
    const bb_serialize_desc_t *desc;
    bb_data_gather_fn          gather;
    void                      *ctx;
    bb_data_replay_kind_t      replay_kind;
} bb_data_binding_t;

// Binds (or re-binds) `binding->key`'s descriptor/gather/ctx. `binding`
// itself may be a stack temporary -- bb_data copies its fields, it does not
// retain the pointer. Re-binding an existing key OVERRIDES its desc/gather/
// ctx in place.
//
// MUST be called only during single-threaded composition, before any
// bb_data_render() traffic starts -- rebinding after render traffic has
// begun is unsupported (no lock guards the binding table; this mirrors
// bb_registry's own composition-time contract).
//
// Returns BB_ERR_INVALID_ARG if `binding`, `binding->key`, `binding->desc`,
// or `binding->gather` is NULL, or if `strlen(binding->key)` is
// >= BB_DATA_KEY_MAX.
// Returns BB_ERR_NO_SPACE if the table is full (BB_DATA_MAX_BINDINGS
// distinct keys already bound) and `binding->key` is not already bound.
bb_err_t bb_data_bind(const bb_data_binding_t *binding);

// Renders `key`'s current value as `fmt` wire bytes. Looks up `key`'s
// binding and `fmt`'s registered renderer (bb_serialize_format_get_render())
// FIRST -- an unsupported format is a true no-op, the gather hook is never
// invoked -- then calls the gather hook into `scratch` (CALLER-OWNED,
// capacity `scratch_cap`, which MUST be >= the binding's desc->snap_size),
// writing the rendered bytes into `buf` (capacity `cap`) and the rendered
// length into `*out_len`.
//
// Returns BB_ERR_INVALID_ARG if `key`, `scratch`, `buf`, or `out_len` is
// NULL.
// Returns BB_ERR_NOT_FOUND if `key` has no binding.
// Returns BB_ERR_UNSUPPORTED if `fmt` has no registered renderer (gather is
// never invoked).
// Returns BB_ERR_NO_SPACE if `scratch_cap` is smaller than the binding's
// desc->snap_size (gather is never invoked), or if the renderer's own
// output overflows `cap`.
// Propagates any error the gather hook itself returns.
bb_err_t bb_data_render(bb_format_t fmt, const char *key,
                        void *scratch, size_t scratch_cap,
                        char *buf, size_t cap, size_t *out_len);

// Returns `key`'s bound replay_kind via `*out_kind`. Not yet consumed by any
// broadcaster (B1-1033 will) -- this PR only stores/reads the flag.
//
// Returns BB_ERR_INVALID_ARG if `key` or `out_kind` is NULL.
// Returns BB_ERR_NOT_FOUND if `key` has no binding.
bb_err_t bb_data_binding_replay_kind(const char *key, bb_data_replay_kind_t *out_kind);

// Per-binding generation counter -- the coherence/invalidation signal for the
// future bidirectional data path (a lost-wakeup ordering anchor for the
// eventual SSE/WS drainer, and a future cache-invalidation hook). This PR
// only adds the counter and its two accessors; nothing consumes it yet.
//
// It is NOT a lock, NOT a pub/sink, and has NO callback/notify surface -- it
// does not wake or signal anything. A producer bumps it via bb_data_touch()
// AFTER durably writing the value its gather hook exposes; a consumer polls
// bb_data_generation() to detect that the value changed since it last read
// it.
//
// The counter is scoped to the binding SLOT, not the key's current target --
// it is NOT reset when a key is re-bound via bb_data_bind() (rebinding
// carries the counter forward). Do not assume a rebind resets generation.

// Bumps `key`'s generation counter by one. A producer calls this AFTER
// durably writing the value it exposes via its gather hook -- bb_data_touch()
// does not itself gather or render anything.
//
// Returns BB_ERR_INVALID_ARG if `key` is NULL.
// Returns BB_ERR_NOT_FOUND if `key` has no binding.
bb_err_t bb_data_touch(const char *key);

// Returns `key`'s current generation counter via `*out_gen`.
//
// Returns BB_ERR_INVALID_ARG if `key` or `out_gen` is NULL.
// Returns BB_ERR_NOT_FOUND if `key` has no binding.
bb_err_t bb_data_generation(const char *key, uint32_t *out_gen);

#ifdef BB_DATA_TESTING
// Test-only: clears the binding table AND the underlying bb_registry
// instance to empty.
void bb_data_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
