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
 *
 * INGRESS (B1-1022) -- the write-half mirror of the render path above. A
 * binding's OPTIONAL `apply` hook (bb_data_binding_t.apply) durably applies
 * an already-scattered snapshot; bb_data_apply() drives the full pipeline
 * (decode wire bytes -> seed dst_scratch -> bb_serialize_populate() ->
 * apply()) against CALLER-OWNED buffers only, exactly like
 * bb_data_render(). bb_data stays HTTP-agnostic throughout: bb_data_apply()
 * returns a bb_err_t, never a status code -- a caller (e.g. an HTTP route)
 * maps that to a transport response itself.
 *
 * PARSE/COMMIT SPLIT (bb_http_section PR) -- bb_data_apply() is a flat
 * bb_err_t, so a caller can never tell "the wire body itself was bad"
 * (400-class) apart from "the body decoded fine but a downstream step
 * rejected/failed it" (400/500-class, depending which step). bb_data_parse()
 * + bb_data_commit() below split the pipeline at exactly that boundary:
 * bb_data_parse() does ONLY binding/format lookup + wire-body decode (no
 * caller-supplied `dst_scratch` in scope at all); bb_data_commit() does
 * everything downstream of a successful decode -- seed, populate, apply().
 * bb_data_apply() itself is now a thin wrapper composing the two, kept
 * around so every pre-existing caller (bb_wifi_http, bb_diag_http's
 * bb_storage_http_routes.c, bb_ota_check) compiles and behaves identically
 * with zero edits.
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

// Args passed to a binding's gather hook on every render: `ctx` is the
// binding's own bind-time context (bb_data_binding_t.ctx, unchanged from
// today); `query` is the CALLING request's query params (or NULL for a
// query-less render, e.g. the SSE broadcaster) -- bb_serialize_query_t
// (see bb_serialize.h), not a bb_data type: a filtered producer (e.g.
// bb_storage's diag fill fn) depends on bb_serialize alone, never bb_data.
// A gather hook MUST NOT mutate any state reachable from `ctx` based on
// `query` -- the render path is reentrant/lock-free BECAUSE per-request
// data flows in as a call arg, never smuggled through bind-time `ctx`.
typedef struct {
    void                        *ctx;
    const bb_serialize_query_t  *query;
} bb_data_gather_args_t;

// Egress hook: fills `dst` (the binding's descriptor's snap_size bytes,
// CALLER-OWNED scratch storage sized by the bb_data_render() caller) from
// live sources. `args` is never NULL; `args->query` may be NULL.
//
// PATCH-SEED CONTRACT (bb_data_commit(), BB_DATA_APPLY_PATCH mode): this
// same hook doubles as the ingress seed step, invoked AFTER a request body
// has already been decoded (bb_data_parse()'s job ran first) -- so unlike a
// render-path call, a PATCH-seed gather() failure can never preempt a
// parse-stage rejection; decode always wins. If a binding is driven through
// BB_DATA_APPLY_PATCH, its gather() SHOULD stay side-effect-free and
// infallible (always return BB_OK) -- e.g. seeding a fixed sentinel, never
// touching hardware/global state or a value that can fail to read. A
// binding whose gather() genuinely mutates state or can fail is fine for
// render/BB_DATA_APPLY_POST use, but should not also be bound as a
// BB_DATA_APPLY_PATCH seed.
typedef bb_err_t (*bb_data_gather_fn)(void *dst, const bb_data_gather_args_t *args);

// Args passed to a binding's apply hook on every bb_data_apply() call --
// mirrors bb_data_gather_args_t's shape (ctx only). apply() is HTTP-agnostic
// and has no request-scoped filter concept, so there is no `query` here.
typedef struct {
    void *ctx;
} bb_data_apply_args_t;

// Ingress hook: durably applies `snap` (the binding's descriptor's
// snap_size bytes, already scattered by bb_data_apply() via
// bb_serialize_populate() against this same binding's desc) through
// `args->ctx`. `args` is never NULL. OPTIONAL on a binding -- a binding with
// gather but no apply is egress-only; bb_data_apply() returns
// BB_ERR_UNSUPPORTED for such a binding without invoking gather or parse
// (see bb_data_apply()'s doc).
//
// Should return BB_ERR_VALIDATION for a domain-level reject (structurally
// well-formed input the destination itself rejects) so an HTTP-agnostic
// caller's err->status mapping stays a simple, uniform switch.
typedef bb_err_t (*bb_data_apply_fn)(const void *snap, const bb_data_apply_args_t *args);

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
    bb_data_apply_fn           apply;  // OPTIONAL -- NULL means egress-only (B1-1022)
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

// The ONE render entry point's request struct -- a config struct, not an
// `_ex` positional-overload variant (workspace convention, mirrors
// bb_data_binding_t). `query` is request-scoped and OPTIONAL: NULL means no
// request params (e.g. the SSE broadcaster's query-less sweep render);
// non-NULL is forwarded to the binding's gather hook via
// bb_data_gather_args_t.query, byte-for-byte, without bb_data itself ever
// interpreting it.
typedef struct {
    bb_format_t                  fmt;
    const char                  *key;
    const bb_serialize_query_t  *query;
    void                        *scratch;
    size_t                       scratch_cap;
    char                        *buf;
    size_t                       buf_cap;
    size_t                      *out_len;
} bb_data_render_req_t;

// Renders `req->key`'s current value as `req->fmt` wire bytes. Looks up
// `req->key`'s binding and `req->fmt`'s registered renderer
// (bb_serialize_format_get_render()) FIRST -- an unsupported format is a
// true no-op, the gather hook is never invoked -- then calls the gather
// hook into `req->scratch` (CALLER-OWNED, capacity `req->scratch_cap`, which
// MUST be >= the binding's desc->snap_size) with a bb_data_gather_args_t
// built from the binding's stored `ctx` plus `req->query`, writing the
// rendered bytes into `req->buf` (capacity `req->buf_cap`) and the rendered
// length into `*req->out_len`. When `req->query` is NULL, behavior is
// byte-identical to a query-less render.
//
// Returns BB_ERR_INVALID_ARG if `req`, `req->key`, `req->scratch`,
// `req->buf`, or `req->out_len` is NULL.
// Returns BB_ERR_NOT_FOUND if `req->key` has no binding.
// Returns BB_ERR_UNSUPPORTED if `req->fmt` has no registered renderer
// (gather is never invoked).
// Returns BB_ERR_NO_SPACE if `req->scratch_cap` is smaller than the
// binding's desc->snap_size (gather is never invoked), or if the renderer's
// own output overflows `req->buf_cap`.
// Propagates any error the gather hook itself returns.
bb_err_t bb_data_render(const bb_data_render_req_t *req);

// ---------------------------------------------------------------------------
// Ingress -- bb_data_apply() (B1-1022), the write-half mirror of
// bb_data_render() above. bb_data_parse()/bb_data_commit() (bb_http_section
// PR) below split it into its two stages; bb_data_apply() itself is a thin
// wrapper composing them, unchanged for every existing caller.
// ---------------------------------------------------------------------------

// Seed mode for bb_data_commit()/bb_data_apply()'s dst_scratch step -- see
// bb_data_commit()'s doc.
typedef enum {
    BB_DATA_APPLY_POST  = 0,  // full replace: dst_scratch is zero-filled
    BB_DATA_APPLY_PATCH = 1,  // partial merge: dst_scratch is seeded via the
                               // binding's own gather() hook first -- a wire
                               // field ABSENT from the body then keeps
                               // whatever value gather() wrote for it
                               // (bb_serialize_populate() never zeroes an
                               // absent field's destination bytes)
} bb_data_apply_mode_t;

// bb_data_parse()'s request struct. `body`/`body_len` is the wire bytes to
// decode; `parse_scratch` (capacity `parse_scratch_cap`) backs the format's
// parse fn and MUST outlive the paired bb_data_commit() call (the resulting
// bb_data_parsed_t borrows into it -- see bb_serialize_parse_fn's own doc).
// Deliberately carries NO `dst_scratch` -- decode is fully independent of
// the eventual destination buffer, which is bb_data_commit()'s concern
// alone.
typedef struct {
    bb_format_t  fmt;
    const char  *key;
    const char  *body;
    size_t       body_len;
    void        *parse_scratch;
    size_t       parse_scratch_cap;
} bb_data_parse_req_t;

// The result of a successful bb_data_parse() call -- CALLER-STACK-OWNED (no
// heap), opaque, and BORROWS into the `parse_scratch` buffer supplied to
// bb_data_parse(): it stays valid only as long as that buffer does, and only
// until the paired bb_data_commit() call consumes it. Do not inspect or copy
// its fields; do not reuse a `bb_data_parsed_t` across more than one
// bb_data_commit() call.
typedef struct {
    void                     *_binding;  // opaque; internal binding-slot handle
    bb_serialize_populate_t   _src;      // decoded source, borrows into parse_scratch
} bb_data_parsed_t;

// Looks up `req->key`'s binding and `req->fmt`'s registered parse fn
// (bb_serialize_format_get_parse()) FIRST, then checks the binding HAS an
// apply hook -- an unbound key, an unsupported format, or an apply-less
// (egress-only) binding is a true no-op: the format's parse fn is never
// invoked. On success, decodes `req->body`/`req->body_len` via that parse fn
// into `req->parse_scratch`, and binds the result (plus the resolved
// binding) into `*out_parsed`. Only binds `*out_parsed` on BB_OK -- on any
// error return, `*out_parsed` is left untouched.
//
// Returns BB_ERR_INVALID_ARG if `req`, `req->key`, `req->parse_scratch`, or
// `out_parsed` is NULL, or if `req->body_len > 0` and `req->body` is NULL.
// Returns BB_ERR_NOT_FOUND if `req->key` has no binding.
// Returns BB_ERR_UNSUPPORTED if `req->fmt` has no registered parse fn, or
// the binding's apply hook is NULL.
// Otherwise propagates whatever the parse fn itself returns (e.g.
// BB_ERR_PARSE_GRAMMAR/BB_ERR_PARSE_INCOMPLETE for a malformed/truncated
// body).
bb_err_t bb_data_parse(const bb_data_parse_req_t *req, bb_data_parsed_t *out_parsed);

// bb_data_commit()'s request struct. `dst_scratch` (capacity
// `dst_scratch_cap`, MUST be >= the parsed binding's desc->snap_size) is the
// destination snapshot struct scattered into and then passed to the
// binding's apply() hook.
typedef struct {
    bb_data_apply_mode_t  mode;
    void                  *dst_scratch;
    size_t                 dst_scratch_cap;
} bb_data_commit_req_t;

// Applies a previously bb_data_parse()'d result to its bound destination via
// the binding's apply hook. Drives:
//   1. seeds `req->dst_scratch` per `req->mode` (see bb_data_apply_mode_t) --
//      BB_DATA_APPLY_PATCH calls the binding's gather() hook with the same
//      ctx a query-less render would use (args->query == NULL).
//   2. scatters `parsed`'s decoded source into `req->dst_scratch` via
//      bb_serialize_populate(desc, dst_scratch, &src).
//   3. calls the binding's apply(dst_scratch, &args) hook.
// Each step only runs if the previous one returned BB_OK; the apply hook is
// NEVER called after a gather/populate failure.
//
// CONTRACT: because this seed step runs AFTER bb_data_parse()'s decode (not
// before, as bb_data_apply()'s single-call predecessor did), a binding's
// gather() hook MUST stay side-effect-free and infallible (always return
// BB_OK) for a BB_DATA_APPLY_PATCH binding whose apply route also needs a
// clean parse/commit boundary -- see bb_data_gather_fn's own doc. A gather()
// that can genuinely fail or has observable side effects is only safe under
// this split if its binding is never driven through bb_data_commit() with a
// body that might itself fail to decode; bb_data_apply()'s composed
// call order below still guarantees decode-before-seed for every caller.
//
// Returns BB_ERR_INVALID_ARG if `parsed`, `parsed`'s internal binding
// handle, `req`, or `req->dst_scratch` is NULL.
// Returns BB_ERR_NO_SPACE if `req->dst_scratch_cap` is smaller than the
// binding's desc->snap_size.
// Otherwise propagates whatever the seed gather() call (PATCH mode only),
// bb_serialize_populate(), or the apply hook itself returns.
bb_err_t bb_data_commit(const bb_data_parsed_t *parsed, const bb_data_commit_req_t *req);

// The ONE apply entry point's request struct -- mirrors bb_data_render_req_t's
// shape/caller-owned-buffers convention. Union of bb_data_parse_req_t's and
// bb_data_commit_req_t's fields (see bb_data_apply()'s doc for how they're
// threaded through).
typedef struct {
    bb_format_t             fmt;
    const char              *key;
    bb_data_apply_mode_t     mode;
    const char              *body;
    size_t                   body_len;
    void                    *parse_scratch;
    size_t                   parse_scratch_cap;
    void                    *dst_scratch;
    size_t                   dst_scratch_cap;
} bb_data_apply_req_t;

// Applies `req->body` (wire bytes in format `req->fmt`) to `req->key`'s
// bound destination via its apply hook. A thin wrapper composing
// bb_data_parse() then bb_data_commit() (see both docs above) -- decodes
// `req->body` FIRST, then seeds/populates/applies. An unbound key, an
// unsupported format, or an apply-less (egress-only) binding is a true
// no-op: gather/parse/populate/apply are never invoked.
//
// Returns BB_ERR_INVALID_ARG if `req`, `req->key`, `req->parse_scratch`, or
// `req->dst_scratch` is NULL, or if `req->body_len > 0` and `req->body` is
// NULL.
// Returns BB_ERR_NOT_FOUND if `req->key` has no binding.
// Returns BB_ERR_UNSUPPORTED if `req->fmt` has no registered parse fn, or
// the binding's apply hook is NULL.
// Returns BB_ERR_NO_SPACE if `req->dst_scratch_cap` is smaller than the
// binding's desc->snap_size.
// Otherwise propagates whatever bb_data_parse() or bb_data_commit() itself
// returns -- see both docs above for the exact propagation rules.
bb_err_t bb_data_apply(const bb_data_apply_req_t *req);

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

// ---------------------------------------------------------------------------
// Scratch acquire/release (B1-1287, the httpd worker task stack-exhaustion
// fix) -- an ADDITIVE, callee-owned pair, NOT a change to bb_data_apply()'s
// caller-owned-buffers contract above (documented three times, deliberate,
// host tests depend on it -- see bb_data_parse_req_t/bb_data_commit_req_t/
// bb_data_apply_req_t's own doc comments). This pair exists purely to shrink
// an httpd worker task's own call-frame: a route handler that would
// otherwise declare its own `char body[N+1]; char parse_scratch[3072];`
// stack locals can instead borrow both from ONE shared, file-scope static
// pair owned by bb_data itself -- confirmed on hardware (controlled A/B) to
// be the difference between a `Stack canary watchpoint triggered` panic and
// a clean POST /api/diag/reboot at CONFIG_BB_HTTP_TASK_STACK_SIZE=6144.
//
// SAFETY (why a single shared static instance is sound here, unlike
// bb_data_render()/bb_data_apply()'s own deliberately reentrant, caller-
// owned-buffer design above): ESP-IDF's httpd spawns exactly ONE worker
// task (`httpd_thread`); httpd_server() walks open sessions sequentially via
// httpd_sess_enum()/httpd_sess_process(), dispatching a synchronous handler
// at a time on THAT one task's own stack -- max_open_sockets/
// lru_purge_enable govern parked CONNECTIONS, not concurrent handler
// invocations. Async handlers exist (bb_http.c's httpd_req_async_handler_
// begin() path), but the sole caller in this codebase is the SSE broadcaster
// (platform/espidf/bb_data_http/bb_data_http_espidf.c), which returns from
// its handler immediately and does its rendering on a SEPARATE broadcaster
// task -- it never calls bb_data_apply()/bb_data_scratch_acquire() at all,
// so it can never collide with this pair.
//
// A caller MUST NOT retain `out`'s pointers past the matching
// bb_data_scratch_release() call, and MUST NOT call bb_data_scratch_acquire()
// again before releasing (see bb_data_scratch_acquire()'s own doc comment
// for the reentrancy guard this trips).
typedef struct {
    char   *body;              // BB_DATA_SCRATCH_BODY_BYTES capacity
    size_t  body_cap;
    void   *parse_scratch;     // BB_DATA_SCRATCH_PARSE_BYTES capacity
    size_t  parse_scratch_cap;
} bb_data_scratch_t;

// Common parse-scratch sizing (see bb_data_parse_req_t's own doc comment and
// its BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP note) -- NOT schema-tunable per
// call site, even for a single-bool schema: the token recorder's own
// default-capacity pool alone is 48 * sizeof(bb_serialize_json_tok_t) ==
// 2304 bytes, so every current caller of this scratch pair already sized its
// own hand-rolled parse_scratch to exactly this many bytes regardless of its
// own schema's real size. Shrinking that global pool is a separate ticket
// (B1-1293), not attempted here.
#define BB_DATA_SCRATCH_PARSE_BYTES 3072

// Sized to the largest current caller's body cap -- DELETE /api/diag/storage
// (BB_STORAGE_HTTP_DELETE_BODY_MAX == 1024, +1 for the NUL terminator).
// Every other current caller's own body cap is smaller and UNAFFECTED by
// this shared sizing: a caller still enforces ITS OWN, narrower cap (e.g.
// the wifi PATCH route's WIFI_PATCH_BODY_MAX) when it reads into
// `body`/`body_cap` -- bb_data_scratch_acquire() only guarantees the buffer
// is AT LEAST `body_cap` bytes big, it never widens what any individual
// caller accepts over the wire.
#define BB_DATA_SCRATCH_BODY_BYTES 1025

// Acquires bb_data's single shared scratch pair for the calling task's
// exclusive use until the matching bb_data_scratch_release() call. Fails
// closed on a nested/concurrent acquire (a second acquire before the first's
// release): always returns BB_ERR_INVALID_STATE without ever handing out the
// (already-claimed) buffers -- and, in a real (non-BB_DATA_TESTING) build,
// additionally trips assert() right at the point of misuse, so a future
// change that routes one of this pair's callers through an async handler
// (or a downstream app that runs two HTTP server instances) is caught
// loudly rather than silently corrupting a concurrent parse. See
// bb_data_scratch_t's own SAFETY doc comment above for why a single shared
// instance is sound today. The assert is compiled out under BB_DATA_TESTING
// so a host test can exercise the fail-closed return path directly -- a
// process-aborting assert() is not something a Unity test can observe as a
// passing result.
//
// Returns BB_ERR_INVALID_ARG if `out` is NULL.
// Returns BB_ERR_INVALID_STATE if the scratch pair is already acquired.
bb_err_t bb_data_scratch_acquire(bb_data_scratch_t *out);

// Releases the scratch pair acquired via bb_data_scratch_acquire(). A stray
// release with nothing currently acquired is a benign no-op -- unlike a
// double-acquire, there is no shared state a double-release could corrupt.
void bb_data_scratch_release(void);

#ifdef BB_DATA_TESTING
// Test-only: clears the binding table AND the underlying bb_registry
// instance to empty.
void bb_data_test_reset(void);

// Test-only: forces the scratch pair back to the released state regardless
// of any outstanding acquire -- lets a test start from a known-clean slate
// without depending on every earlier test in the same binary having
// released it. Mirrors bb_data_test_reset()'s own reset-between-tests
// posture.
void bb_data_scratch_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
