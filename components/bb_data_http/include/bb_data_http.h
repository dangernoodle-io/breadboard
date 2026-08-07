#pragma once

/**
 * @brief bb_data_http -- the converged HTTP SSE/WS push transport (B1-1033,
 * design KB 1443/1444). Dep-light pure core (bb_queue + bb_core only): it
 * NEVER links bb_data or bb_ws_server directly. Instead it calls three
 * INJECTED function-pointer seams -- render, generation-read, and send --
 * that the composition root wires to real bb_data / bb_ws_server / httpd
 * calls (ESP-IDF) or to test doubles (host). This keeps bb_data_http free to
 * serve any egress transport without a hard dependency on the data or
 * websocket layers.
 *
 * Host-testable pure core: no FreeRTOS task, no httpd, no real sockets --
 * the espidf backend that drives bb_data_http_sweep_step() from a
 * broadcaster task is a later, HW-gated PR (B1-1045).
 *
 * EVENT replay (B1-1032, this PR): a single SHARED bb_queue ring, fed by
 * EVENT-kind attached keys, holds rendered frames in monotonic push order.
 * Each active client tracks its own push->cursor (the next undrained global
 * push-sequence number) and independently drains ring entries -- newer than
 * its cursor and matching its topic_filter -- into its own outbound queue
 * each sweep_step(). Unlike STATE (coalesced dirty-mask, latest value wins),
 * EVENT is append-only: every detected generation change on an EVENT-kind
 * key produces at most one ring push, and a slow client never causes another
 * client's delivery to stall (the ring is never blocked on a reader). If a
 * client's own outbound queue has no room for a drained event, that event is
 * DROPPED for that client only (never evicted from the shared ring), its
 * per-client dropped counter increments, and a "dropped:N" marker frame is
 * queued for it as soon as outbound has room again -- see
 * bb_data_http_client_dropped_count() and bb_data_http_sweep_step().
 *
 * Composition-root-owned attach table: bb_data_http_attach() maps a bb_data
 * key to a topic name. bb_data itself stays topic-agnostic -- the key<->topic
 * mapping lives entirely here. A client's topic_filter (see
 * bb_data_http_client_acquire's bb_data_http_client_cfg_t) selects which
 * attached keys it receives: NULL/"" subscribes to every attached key; a
 * non-empty filter subscribes only to keys attached under that exact topic
 * name (bb_data_http_attach_cfg_t's `topic` field).
 *
 * HARD INVARIANT (the STATE lost-wakeup guarantee, KB 1443): the sweep-step's
 * detect phase writes every dirty client's poll->seen_gen to the generation
 * it observed, and runs to COMPLETION for every attached key/client BEFORE
 * any render_fn call happens that same pass. A generation bump that lands
 * during (or after) this pass's own render calls is therefore invisible to
 * THIS pass's detect step and is correctly re-detected by the NEXT
 * sweep_step() call instead of being silently absorbed -- this two-phase
 * split (detect fully precedes drain), not the per-key clear/render
 * ordering, is what actually prevents lost wakeups. The drain phase's own
 * clear-only-on-render-success ordering (see bb_data_http_sweep_step() below)
 * instead governs a different failure mode: a render_fn that fails must not
 * silently drop the key's update, so the dirty bit stays set for a retry on
 * the next sweep.
 */

#include "bb_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to an acquired SSE/WS client slot.
typedef struct bb_data_http_client bb_data_http_client_t;

// ---------------------------------------------------------------------------
// Replay kind (B1-1032) -- both STATE (coalesced dirty-mask) and EVENT
// (shared ring + per-client cursor) are fully implemented.
// ---------------------------------------------------------------------------
typedef enum {
    BB_DATA_HTTP_STATE = 0,
    BB_DATA_HTTP_EVENT = 1,
} bb_data_http_replay_kind_t;

// Kind-subscription bitmask (B1-1445, epic B1-1123). Bit `1u <<
// slot->kind` for each bb_data_http_replay_kind_t a client wants delivered
// to it -- see bb_data_http_client_cfg_t's `subscribe_mask` doc below for
// the full contract.
#define BB_DATA_HTTP_SUBSCRIBE_STATE (1u << BB_DATA_HTTP_STATE)
#define BB_DATA_HTTP_SUBSCRIBE_EVENT (1u << BB_DATA_HTTP_EVENT)
#define BB_DATA_HTTP_SUBSCRIBE_ALL   (BB_DATA_HTTP_SUBSCRIBE_STATE | BB_DATA_HTTP_SUBSCRIBE_EVENT)

// The macros above hard-code BB_DATA_HTTP_STATE/EVENT's numeric values (0/1)
// rather than deriving the shift from the enumerator itself -- this assert
// is what keeps that safe: inserting a kind before EVENT, or dropping either
// enumerator's explicit `= N`, would silently invert which bit gates which
// kind (a client's mask would gate the wrong delivery path with no compile
// error) were this not here.
_Static_assert(BB_DATA_HTTP_STATE == 0 && BB_DATA_HTTP_EVENT == 1,
               "subscribe-mask bit mapping depends on these enum values");

// Max length (including NUL) of an attach-table topic name or a client's
// topic_filter. Fixed compile-time constant (mirrors bb_data's
// BB_DATA_KEY_MAX sizing rationale) -- not Kconfig-tunable.
#define BB_DATA_HTTP_TOPIC_MAX 32

// Max length (including NUL) of a bb_data key as stored in the attach table.
// Independent of (but numerically mirrors) bb_data's BB_DATA_KEY_MAX -- this
// component has no compile dependency on bb_data.
#define BB_DATA_HTTP_KEY_MAX 48

// Max number of key->topic attach-table entries. One bb_data key is attached
// at most once, so this is sized to bb_data's own binding-table capacity
// without creating a build dependency on bb_data.h.
#define BB_DATA_HTTP_MAX_ATTACH 8

// ---------------------------------------------------------------------------
// Injected seams -- the ONLY way this component touches rendering, coherence
// tracking, or transport I/O. Never called with ctx swapped between seams.
// ---------------------------------------------------------------------------

// Renders `key`'s current value into `buf` (capacity `cap`); `*out_len`
// receives the rendered length on BB_OK. `ctx` is the opaque pointer passed
// to bb_data_http_set_render_fn(). The real (later) backend wraps
// bb_data_render(); host tests supply a fake.
typedef bb_err_t (*bb_data_http_render_fn)(const char *key, char *buf, size_t cap,
                                            size_t *out_len, void *ctx);

// Reads `key`'s current coherence/generation counter into `*out_gen`. `ctx`
// is the opaque pointer passed to bb_data_http_set_generation_fn(). The real
// backend wraps bb_data_generation(); host tests supply a fake. Injected
// (rather than a direct bb_data_generation() call) so this component never
// links bb_data -- see the file header.
//
// CONTRACT (B1-1449): this callback MUST be a pure, side-effect-free read,
// with its result for any given key independent of call ORDER relative to
// any other key -- never lazily computed, cached, or mutated as a side
// effect of being called. bb_data_http_sweep_step()/bb_data_http_push_pump()
// rely on this: since the B1-1449 split, every STATE key's generation is now
// read before any EVENT key's (see bb_data_http_push_pump()'s own doc) --
// a call-order change from the pre-split single interleaved pass. That
// reordering is observably inert ONLY because this contract holds; an
// implementation that violates it (e.g. one that resets some shared
// "dirty" state as a side effect of being read) would see this reordering
// become a silent, real behavior change.
typedef bb_err_t (*bb_data_http_generation_fn)(const char *key, uint32_t *out_gen, void *ctx);

// Sends `len` already-rendered bytes to `client` (the same handle returned
// by bb_data_http_client_acquire()). Transport-neutral by design (B1-1123
// PR-1): the seam carries no fd/is_ws -- those were HTTP-specific and meant
// nothing to a future MQTT/UDP backend. A backend that needs socket/framing
// detail (e.g. the ESP-IDF SSE backend) resolves it itself, either from its
// own side table keyed on `client` or, since bb_data_http_client_t is a
// component-private struct shared with platform backends via
// bb_data_http_internal.h, straight off `client` itself (see
// platform/espidf/bb_data_http/bb_data_http_espidf.c's espidf_send_fn and
// platform/host/bb_data_http/bb_data_http_host.c's host_send). `ctx` is the
// opaque pointer passed to bb_data_http_set_send_fn() -- one shared value for
// every call installed this way, but see the PER-CONSUMER SEAMS doc below:
// a client acquired with its own send_fn/send_ctx (bb_data_http_client_cfg_t)
// uses THAT ctx instead, never this module-wide one. The real backend wraps
// a socket write (or bb_ws_server send); host tests supply a capture stub
// (platform/host/bb_data_http/bb_data_http_host.h).
//
// `key` (B1-1123 PR-2) answers "what is being written", complementing
// `client`/`ctx`'s "who to write to" -- a future MQTT consumer needs it to
// pick a topic; it is orthogonal to the client-identity question and does
// NOT replace it (SSE still needs `client` to pick a socket). `key` is
// ALWAYS a valid, non-NULL, NUL-terminated string, even for the internal
// drop-marker frame (bb_data_http_sweep_step()'s EVENT drop-marker doc),
// which uses a reserved literal rather than NULL -- so a consumer can always
// treat `key` as a plain C string with no NULL-check.
//
// Return contract (B1-1424/B1-1429): the return value is not just
// success/failure -- it tells the core's flush loop (bb_data_http_sweep_step())
// whether a retry is SAFE, and the core cannot infer that on its own (it has
// no socket/transport knowledge of its own). Exactly two classes:
//
//   - BB_OK: delivered (a synchronous transport like SSE) or successfully
//     handed off (an async-enqueue transport like WS -- see
//     bb_data_http_espidf.c's espidf_send_fn WS branch doc for what BB_OK
//     does and doesn't guarantee there).
//   - BB_ERR_TIMEOUT: RETRIABLE -- send_fn is certifying that NOTHING was
//     transmitted for this call (a pure enqueue/would-block failure with no
//     partial bytes on the wire, e.g. a WS async broadcast that failed to
//     queue). The core leaves the frame at the head of the queue and retries
//     it on the next sweep_step() call, bounded by
//     CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX consecutive failures (see
//     bb_data_http_sweep_step()'s flush doc).
//   - Any other non-BB_OK code: FATAL -- send_fn cannot rule out that some
//     bytes already reached the peer with no way to resync (e.g. a chunked-
//     HTTP write that fails partway through a multi-write header/body
//     sequence, where the underlying framing protocol has no resync token --
//     see B1-1429's SSE preamble post-mortem). The core does NOT retry a
//     fatal failure and does NOT keep draining this client's queue -- it
//     tears the client down immediately via the installed abort_fn (see
//     bb_data_http_set_abort_fn() below), the same teardown a dead-peer
//     liveness probe would perform. Retrying at any position after a fatal
//     failure risks re-sending already-transmitted bytes on top of what is
//     already on the wire, corrupting the stream for the rest of the
//     connection's life.
//
// A transport that cannot tell "nothing transmitted" apart from "some bytes
// transmitted" for a given failure MUST treat it as fatal (the default,
// non-BB_ERR_TIMEOUT case) -- guessing retriable when unsure is the unsafe
// direction.
typedef bb_err_t (*bb_data_http_send_fn)(const char *key, const bb_data_http_client_t *client,
                                          const void *bytes, size_t len, void *ctx);

// Notifies the backend that `client` must be torn down immediately following
// a FATAL send_fn failure (see bb_data_http_send_fn's return contract above)
// -- called at most once per client per bb_data_http_sweep_step() call, from
// the flush loop, never from any other context. `ctx` is the opaque pointer
// passed to bb_data_http_set_abort_fn(), unless `client` was acquired with
// its own abort_fn/abort_ctx (bb_data_http_client_cfg_t), which takes
// precedence -- see the PER-CONSUMER SEAMS doc below.
//
// Implementations own the FULL teardown: transport-specific side-table
// cleanup (e.g. the espidf backend's fd/async_req slot) plus the underlying
// connection abort, THEN bb_data_http_client_release(client) -- mirroring
// exactly what a dead-peer liveness probe already does (see
// bb_data_http_espidf.c's peer_liveness_prepass()/teardown_client()). The
// core deliberately does NOT call bb_data_http_client_release() itself
// before invoking this seam: releasing the client_t slot here first (while
// a backend's own side table still points at it) would let a NEW connection
// reuse the same slot while the old backend-side-table entry is still live,
// aliasing two unrelated connections onto one bb_data_http_client_t.
//
// Missing (fn=NULL -- e.g. a host test exercising only send_fn with no real
// transport) degrades gracefully: the core calls
// bb_data_http_client_release(client) itself instead.
typedef void (*bb_data_http_abort_fn)(bb_data_http_client_t *client, void *ctx);

// Install/replace the render seam. Passing fn=NULL disables rendering (dirty
// keys are silently skipped -- see bb_data_http_sweep_step()).
void bb_data_http_set_render_fn(bb_data_http_render_fn fn, void *ctx);

// Install/replace the generation-read seam. Passing fn=NULL disables STATE
// dirty-detection entirely (no key is ever marked dirty).
void bb_data_http_set_generation_fn(bb_data_http_generation_fn fn, void *ctx);

// Install/replace the MODULE-WIDE default send seam. Passing fn=NULL
// disables draining for every client that has no per-client override (see
// bb_data_http_client_cfg_t below) -- rendered bytes accumulate in the
// per-client outbound queue until a send_fn resolves for it.
void bb_data_http_set_send_fn(bb_data_http_send_fn fn, void *ctx);

// Install/replace the MODULE-WIDE default fatal-abort seam (see
// bb_data_http_abort_fn's doc above). Passing fn=NULL reverts to the core's
// own bb_data_http_client_release()-only fallback for every client that has
// no per-client override (see bb_data_http_client_cfg_t below).
void bb_data_http_set_abort_fn(bb_data_http_abort_fn fn, void *ctx);

// ---------------------------------------------------------------------------
// PER-CONSUMER SEAMS (B1-1123 PR-1). render_fn/generation_fn above stay
// module-wide singletons -- both are pure bb_data accessors (key -> bytes,
// key -> generation) with no transport identity, so every consumer wants the
// identical answer for a given key. send_fn/abort_fn are different: they ARE
// transport identity (a socket write, an MQTT publish, a UDP send), so a
// second consumer installing its own send_fn via bb_data_http_set_send_fn()
// would silently evict the first module-wide one -- the singleton bug this
// PR fixes. Each client slot now carries its OWN optional send_fn/send_ctx/
// abort_fn/abort_ctx, set once at bb_data_http_client_acquire() time via
// bb_data_http_client_cfg_t below and resolved by the flush loop as
// `c->send_fn ? c->send_fn : (module-wide s_send_fn)` (and likewise for
// abort_fn) -- a client with no override transparently falls back to
// whatever bb_data_http_set_send_fn()/bb_data_http_set_abort_fn() installed,
// so existing HTTP callers (which never set these cfg fields) are unchanged.
// The attach table, describe table, and shared EVENT ring stay global too --
// none carries transport identity; key->topic is a bb_data concept, not a
// per-consumer one.
// ---------------------------------------------------------------------------

// Config struct for bb_data_http_client_acquire() -- the single-params-struct
// convention (never `_ex`/`_named`): this REPLACES the old
// bb_data_http_client_acquire()/bb_data_http_client_acquire_ex() pair rather
// than adding a third entry point.
//
// B1-1448 (epic B1-1123): no socket-shaped field lives here. fd/is_ws were
// the last HTTP-specific fields on this cfg -- carrying them asserted every
// consumer was a socket, which is exactly what a future MQTT/UDP consumer is
// not. A transport that needs socket/framing identity resolves it itself,
// from its OWN side table keyed on the acquired bb_data_http_client_t* (see
// platform/espidf/bb_data_http/bb_data_http_espidf.c's per-connect-site
// s_slots[]/s_ws_slots[]), never through this cfg.
//
// topic_filter: NULL/"" subscribes to every attached key; a non-empty filter
// subscribes only to keys attached under that exact topic name
// (bb_data_http_attach()'s `topic` argument). Zero-default (NULL) is safe --
// "subscribe to everything" is the same behavior an omitted field already
// had under the old convenience wrapper.
// send_fn/send_ctx: per-client send seam (see the PER-CONSUMER SEAMS doc
// above). NULL means "use the module-wide default installed via
// bb_data_http_set_send_fn()" -- the safe zero-default every existing HTTP
// call site relies on.
// abort_fn/abort_ctx: per-client fatal-abort seam, same NULL-means-
// module-wide-default rule as send_fn/send_ctx above.
// subscribe_mask (B1-1445): which bb_data_http_replay_kind_t(s) this
// client receives, as a bitmask of BB_DATA_HTTP_SUBSCRIBE_STATE /
// BB_DATA_HTTP_SUBSCRIBE_EVENT (or BB_DATA_HTTP_SUBSCRIBE_ALL for both).
// Gates DELIVERY only -- a key must still match topic_filter (the existing
// topic-string rule) AND have its kind bit set here; the two are ANDed, not
// substitutes for each other. Zero-default (an unset/zero-valued field) is
// safe and resolves to BB_DATA_HTTP_SUBSCRIBE_ALL, matching the behavior
// every existing call site (which never sets this field) already had -- a
// bitmask has no valid-looking zero to collide with, so a bare 0
// unambiguously means "caller didn't ask for a kind filter".
typedef struct {
    const char            *topic_filter;
    bb_data_http_send_fn   send_fn;
    void                  *send_ctx;
    bb_data_http_abort_fn  abort_fn;
    void                  *abort_ctx;
    uint32_t               subscribe_mask;
} bb_data_http_client_cfg_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

typedef struct {
    size_t max_clients;         // 0 -> CONFIG_BB_DATA_HTTP_MAX_CLIENTS
    size_t event_ring_capacity; // 0 -> CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY
    size_t max_poll_clients;    // 0 -> CONFIG_BB_DATA_HTTP_MAX_POLL_CLIENTS (B1-1447)
    size_t max_push_clients;    // 0 -> CONFIG_BB_DATA_HTTP_MAX_PUSH_CLIENTS (B1-1447)
} bb_data_http_cfg_t;

// Initialize the transport core. Idempotent; a second call returns BB_OK.
// cfg=NULL uses Kconfig defaults.
//
// max_poll_clients/max_push_clients (B1-1447): shrink-only overrides of the
// compile-time BB_DATA_HTTP_MAX_POLL_CLIENTS/BB_DATA_HTTP_MAX_PUSH_CLIENTS
// pool caps -- same "may only shrink, never grow" contract as max_clients
// above. Sizes the pools bb_data_http_client_acquire() draws a client's
// STATE/EVENT bookkeeping block from (see bb_data_http_client_cfg_t's
// subscribe_mask doc below); either pool can be smaller than max_clients,
// in which case acquiring more STATE-including (or EVENT-including) clients
// than that pool holds fails BB_ERR_NO_SPACE even while fd-table client
// slots remain free -- see bb_data_http_client_acquire()'s own doc.
// Returns BB_ERR_INVALID_ARG if either exceeds its own Kconfig cap.
bb_err_t bb_data_http_init(const bb_data_http_cfg_t *cfg);

// ---------------------------------------------------------------------------
// Attach table (composition-root owned)
// ---------------------------------------------------------------------------

// Config struct for bb_data_http_attach() -- the single-params-struct
// convention (never `_ex`/`_sized`): this REPLACES the old
// bb_data_http_attach()/bb_data_http_attach_ex()/bb_data_http_attach_sized()
// three-rung ladder rather than adding a fourth entry point (B1-1439, epic
// B1-1434).
//
// key/topic: same NULL/empty/length-bound contract the old ladder shared --
// see bb_data_http_attach()'s own doc below.
// kind: zero-default (BB_DATA_HTTP_STATE == 0, see the enum above) is safe
// -- an omitted field reproduces exactly what the old base bb_data_http_
// attach() hardcoded (BB_DATA_HTTP_STATE), so every former base-call site
// converts by simply omitting this field.
// snap_size: zero-default is safe -- 0 can never exceed
// CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES (Kconfig `range 32 2048`, so the
// scratch bound is never itself 0), so an omitted field never trips the
// loud attach-time guard below, exactly matching the old base attach()/
// attach_ex() pair, which had no snap_size parameter (and therefore no
// guard) at all. Callers that have a bb_serialize_desc_t (the composition
// root does, via bb_data_binding_t.desc) should set this field to
// `desc->snap_size` so an oversized binding is caught at wire-up instead of
// silently render-failing on every sweep (bb_data_render() returns
// BB_ERR_NO_SPACE whenever the caller's scratch is smaller than the
// binding's desc->snap_size -- see bb_data.h).
typedef struct {
    const char                 *key;
    const char                 *topic;
    bb_data_http_replay_kind_t  kind;
    size_t                       snap_size;
} bb_data_http_attach_cfg_t;

// Attach `cfg->key` (a bb_data binding key) under `cfg->topic`, with replay
// kind `cfg->kind` (see the zero-default doc above). Idempotent per key --
// re-attaching an already-attached key updates its topic/kind in place.
//
// The loud attach-time guard (B1-1045 PR-4 fix): rejects the attach
// (BB_ERR_NO_SPACE, ESP_LOGE-level "attach-time fail loud" log) if
// `cfg->snap_size` exceeds CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES instead
// of silently attaching a key whose every future render would fail forever.
// This check runs BEFORE key/topic validation below, so an oversized
// `cfg->snap_size` is reported even when `cfg->key`/`cfg->topic` are also
// invalid.
//
// Returns BB_ERR_INVALID_ARG if `cfg` is NULL, or `cfg->key`/`cfg->topic` is
// NULL/empty, or either exceeds its buffer bound (BB_DATA_HTTP_KEY_MAX /
// BB_DATA_HTTP_TOPIC_MAX).
// Returns BB_ERR_NO_SPACE if `cfg->snap_size` exceeds the render scratch
// bound (the key is NOT attached in this case), or if the attach table is
// full (BB_DATA_HTTP_MAX_ATTACH distinct keys already attached) and `key` is
// not already attached.
bb_err_t bb_data_http_attach(const bb_data_http_attach_cfg_t *cfg);

// Diagnostics: number of keys currently attached.
size_t bb_data_http_attach_count(void);

// ---------------------------------------------------------------------------
// Describe table (B1-1220) -- topic-schema seam for bb_openapi's SSE oneOf
// synthesis. DELIBERATELY INDEPENDENT of the attach table above: many topic
// producers push a schema for OpenAPI documentation purposes without ever
// being bb_data_http_attach()-ed (a composition root can attach a subset of
// bb_data-bound topics while still documenting every schema a client might
// see over some OTHER delivery path) -- coupling schema registration to
// attach would silently start/stop SSE delivery as a side effect of a
// documentation-only call, which is exactly backwards. bb_openapi walks this
// table (bb_openapi_emit_shared.c) in addition to its own legacy
// bb_openapi_register_topic_schema() registry; see that component's header
// for the union's ordering contract.
// ---------------------------------------------------------------------------

// Max number of describe-table entries. Own cap, NOT shared with
// BB_DATA_HTTP_MAX_ATTACH -- the two tables serve different producers and
// have no reason to move in lockstep.
#define BB_DATA_HTTP_MAX_DESCRIBE 8

// Callback invoked once per described entry by bb_data_http_describe_foreach().
// All four string pointers are the exact static-storage pointers passed to
// bb_data_http_describe() -- never copied, never NUL by contract.
typedef void (*bb_data_http_describe_cb_t)(const char *key, const char *topic,
                                           const char *component_name,
                                           const char *schema_literal, void *ctx);

// Registers `key`'s OpenAPI topic schema. All four string arguments must be
// static/persistent storage (pointers are stored, never copied) -- mirrors
// bb_openapi_register_schema()'s own pointer-storage contract.
//
// Dedup first-wins: re-describing an already-described `key` is a no-op that
// returns BB_OK without updating the stored topic/component_name/
// schema_literal.
//
// Returns BB_ERR_INVALID_ARG if any argument is NULL or empty.
// Returns BB_ERR_NO_SPACE if the describe table is full
// (BB_DATA_HTTP_MAX_DESCRIBE distinct keys already described) and `key` is
// not already described.
bb_err_t bb_data_http_describe(const char *key, const char *topic,
                               const char *component_name,
                               const char *schema_literal);

// Walks every described entry in registration order, calling `cb` once per
// entry with `ctx`. No-op if `cb` is NULL.
void bb_data_http_describe_foreach(bb_data_http_describe_cb_t cb, void *ctx);

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------

// Acquire a client slot per `cfg` (see bb_data_http_client_cfg_t's own doc
// above for each field's meaning and zero-default safety). Every STATE key
// the client is subscribed to is marked dirty immediately
// (fresh-render-on-connect) so the first bb_data_http_sweep_step() call
// after acquire renders and sends it.
//
// Returns BB_ERR_INVALID_ARG if `cfg` or `out` is NULL; or if
// `cfg->topic_filter` is non-NULL/non-empty and its length (excluding NUL)
// is >= BB_DATA_HTTP_TOPIC_MAX -- mirrors bb_data_http_attach()'s own
// topic bound so an over-length filter is rejected rather than silently
// truncated (a truncated filter can mis-subscribe a client to the wrong
// topic).
// Returns BB_ERR_INVALID_STATE if bb_data_http_init() has not been called.
// Returns BB_ERR_NO_SPACE if every client slot is in use, OR (B1-1447) if
// `cfg->subscribe_mask` requires a STATE or EVENT bookkeeping block (see
// bb_data_http_cfg_t's max_poll_clients/max_push_clients doc above) and that
// kind's pool is exhausted -- a smaller-than-max_clients pool can run out
// even while an fd-table client slot is still free. This never returns a
// client with an unexpectedly-NULL poll/push pointer: acquisition fails
// cleanly instead, and `*out` is left untouched (the caller's prior value,
// typically NULL, is never overwritten on any failure path).
bb_err_t bb_data_http_client_acquire(const bb_data_http_client_cfg_t *cfg,
                                     bb_data_http_client_t **out);

// Release a client slot: destroys its outbound queue and frees the slot.
// Safe to call with NULL (no-op).
//
// NOT CROSS-TASK-SAFE (B1-1424 HIGH fix): must only ever be called from the
// single task that also calls bb_data_http_sweep_step() (see
// bb_data_http_client_t's TASK OWNERSHIP doc, bb_data_http_internal.h) --
// every field it touches (destroying `outbound`, clearing `in_use`) is
// exclusively owned by that one task, with no lock protecting it, so a
// second task calling this concurrently on the SAME client races the
// owning task's own in-flight sweep_step() work on it. A foreign task that
// needs a client released (e.g. an async transport's own disconnect
// callback, which typically fires on a different task than the one driving
// sweep_step()) MUST call bb_data_http_client_request_release() instead --
// see its doc immediately below.
void bb_data_http_client_release(bb_data_http_client_t *c);

// Cross-task-safe alternative to bb_data_http_client_release() above: marks
// `c` for release on the OWNING task's next bb_data_http_sweep_step() call
// (a deferred reap, not an immediate release) instead of mutating any of
// `c`'s other fields itself. Safe to call from ANY task, including one
// other than the task that owns sweep_step() -- this is the one operation
// on a bb_data_http_client_t explicitly designed to be cross-task-safe (see
// bb_data_http_client_t's TASK OWNERSHIP doc). Safe to call with NULL
// (no-op). Idempotent -- calling this more than once on the same client
// before it is reaped has no additional effect.
//
// A client marked this way stays fully valid (still counted by
// bb_data_http_active_client_count(), still drained by
// bb_data_http_sweep_step() for whatever remains of the CURRENT call in
// progress, if any) until the owning task's NEXT sweep_step() call, which
// reaps it (calls bb_data_http_client_release() itself, single-task) before
// doing any other work for that client that sweep.
void bb_data_http_client_request_release(bb_data_http_client_t *c);

// Diagnostics: number of client slots currently in use. A client with
// pending_release set (bb_data_http_client_request_release()) still counts
// here until the owning task's next bb_data_http_sweep_step() reaps it.
size_t bb_data_http_active_client_count(void);

// ---------------------------------------------------------------------------
// Sweep (pure, synchronous, no task/blocking -- caller drives the cadence)
// ---------------------------------------------------------------------------

// One sweep pass over every attached STATE key and every active client:
//
//   1. Detect: for each attached STATE key, read its current generation
//      (via the injected generation_fn) and compare it against each
//      matching client's own per-key poll->seen_gen. On a mismatch, mark
//      that client's dirty bit for the key AND record the observed
//      generation into poll->seen_gen -- BEFORE any rendering happens this
//      pass. This is what makes the ordering invariant below race-safe: a
//      generation bump that happens *during* this pass's own render call
//      (below) is invisible to this detect step (poll->seen_gen already
//      captured the pre-render value), so it is correctly re-detected on
//      the NEXT sweep_step() call rather than silently absorbed.
//   2. Drain: for each client -- FIRST reaping (bb_data_http_client_release())
//      any client with pending_release set (bb_data_http_client_request_release(),
//      the cross-task-safe deferred-release request -- see its doc) and
//      skipping the rest of this loop body for it, before touching any of
//      its other state -- then, for each dirty bit (in ascending attach-index
//      order): call render_fn, and clear the bit ONLY if render_fn succeeds,
//      THEN push the rendered bytes into that client's outbound bb_queue. A
//      missing render_fn (see the setter above) clears the bit unconditionally
//      -- there is nothing to retry. A render_fn that returns an error leaves
//      the bit set so the key is retried on the next sweep_step() call rather
//      than silently dropped; each such failure increments the counter read
//      by bb_data_http_render_fail_count() and is logged at a rate-limited
//      cadence. Once every dirty key has been drained into the queue, the
//      queue is flushed via send_fn -- see the flush contract below
//      (B1-1424/B1-1429).
//
// EVENT-kind attached keys follow a separate, append-only path within the
// same sweep_step() call:
//
//   1. Ring feed: bb_data_http_push_pump() (see its own doc below) --
//      called here in the same relative position its logic used to occupy
//      inline (B1-1449) -- for each attached EVENT key whose generation
//      (via generation_fn) differs from the module's own last-seen
//      generation for that key, render_fn is called immediately (not
//      deferred to a drain phase -- the shared ring has no per-client dirty
//      state to coalesce against) and the rendered bytes are pushed onto
//      the single shared EVENT ring. The last-seen generation for that key
//      advances ONLY on render success (mirrors STATE's
//      clear-only-on-success retry contract) so a failing render_fn is
//      retried next call rather than silently skipping the event.
//   2. Per-client drain: for each active, subscribed client, every ring
//      entry newer than its push->cursor is drained into its own outbound
//      queue and its cursor advances past it. If the ring itself has
//      evicted entries older than a client's cursor (a slow client that
//      fell behind the ring's own capacity), that gap counts as dropped for
//      that client too. A client whose outbound has no room for a drained
//      event does NOT stall the ring for other clients -- the event is
//      dropped for that client only, its dropped counter increments, and a
//      "dropped:N" marker frame is queued for it once outbound has room
//      again (see bb_data_http_client_dropped_count()).
//
// Flush (B1-1424/B1-1429): each client's outbound queue is drained
// oldest-first, one send_fn call per queued frame, for as long as each call
// keeps succeeding -- a client with several frames queued can still catch
// up within a single sweep_step() call. send_fn's return (see
// bb_data_http_send_fn's own doc, bb_data_http.h) drives one of three
// outcomes:
//
//   - BB_OK: the head-of-queue frame is popped, send_fail_count resets to
//     0, and the NEXT queued frame (if any) is attempted immediately,
//     within the same sweep_step() call.
//   - BB_ERR_TIMEOUT (retriable): the frame stays at the head of the queue
//     (never popped) for a retry on the NEXT sweep_step() call, and
//     send_fail_count increments; this client's flush STOPS for the rest
//     of THIS sweep_step() call (never looped on immediately), which
//     preserves delivery order (a later frame must never reach the wire
//     ahead of one still queued in front of it) and bounds one stalled
//     client's per-sweep cost. Bounded per client: once
//     CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX consecutive retriable failures land
//     on the SAME head-of-queue frame, that one frame is dropped (popped
//     without a further attempt) and send_fail_count resets to 0 -- but
//     the flush STILL stops for this sweep rather than immediately
//     attempting the newly-head frame right here (a worst case of at most
//     one drop-then-stop per client per sweep_step() call, not a
//     queue-depth-many cascade of attempts).
//   - Any other non-BB_OK code (fatal): the frame is left untouched (not
//     popped -- irrelevant, since the client is about to be torn down),
//     this client's flush stops, and the client is released immediately
//     via the installed abort_fn (see bb_data_http_set_abort_fn()), or
//     bb_data_http_client_release() directly if no abort_fn is installed.
//     No retry, no further send_fn calls for this client this sweep or
//     ever again on this connection -- see bb_data_http_send_fn's doc for
//     why a fatal failure can never be safely retried at any position.
//
// What a BB_OK send_fn return means is transport-defined: a synchronous
// backend (e.g. SSE) reports a real delivery verdict, while an async-enqueue
// backend (e.g. WS) only reports "the write was queued", not "the peer
// received it" -- see bb_data_http_send_fn's own doc and
// platform/espidf/bb_data_http/bb_data_http_espidf.c's espidf_send_fn.
//
// A missing render_fn/generation_fn/send_fn degrades gracefully (see the
// setters above) rather than crashing -- useful for host tests exercising
// only one half of the pipeline at a time.
void bb_data_http_sweep_step(void);

// EVENT ring-feed step (B1-1449, epic B1-1123), extracted out of
// bb_data_http_sweep_step()'s own detect phase so it can be called directly
// -- feeding the shared EVENT ring no longer has to wait for the next 50ms
// broadcaster tick. Scans every attached EVENT-kind key for a generation
// change (via the installed generation_fn), renders any that changed (via
// the installed render_fn), and pushes the rendered bytes onto the single
// shared EVENT ring -- see bb_data_http_sweep_step()'s own EVENT doc above
// for the exact per-key semantics (retry-on-render-failure, degrade
// gracefully with no render_fn, etc.), which this function implements
// unchanged.
//
// sweep_step() itself calls this in the same relative position the old
// inline ring-feed logic occupied (after STATE detect, before the
// per-client drain/flush loop) -- observable OUTPUT is unchanged from before
// this function existed; calling it is purely additive. One CALL-ORDER
// detail did change, not just output: pre-split, generation_fn was called
// for every attached key in a single registry-order pass, interleaving
// STATE and EVENT keys; post-split, sweep_step()'s own detect loop calls
// generation_fn for every STATE key first, THEN this function makes a
// second, independent pass calling it for every EVENT key -- so all STATE
// generation reads now precede all EVENT ones, whereas before they were
// interleaved. This is safe ONLY because of generation_fn's own contract
// (see bb_data_http_generation_fn's doc above) requiring it to be a pure,
// order-independent read; ring push order is unaffected either way (still
// strictly increasing attach index k).
//
// SCOPE FENCE: this function decouples ONLY the ring FEED from the tick.
// Draining the ring into any client's own outbound queue, and the outbound
// flush/retry state machine, both stay tick-driven, inside
// bb_data_http_sweep_step() only -- they are NOT exposed as independently
// callable, and must not be. That is what preserves the single-broadcaster-
// task ownership model of every other field this component's clients carry
// (see bb_data_http_client_t's TASK OWNERSHIP doc) -- the model that fixed
// the original per-client-task TCB-reuse hazard this component's ring
// design replaced. Do not add a drain-pump or flush-pump counterpart.
//
// MULTI-WRITER CONTRACT (B1-1450, epic B1-1123): this function is safe to
// call from a second, concurrent task alongside the existing broadcaster
// task -- e.g. a dedicated producer task calling this function directly,
// without going through sweep_step() at all. Safety comes from a
// CLAIM/RENDER/COMMIT-OR-REVOKE protocol around the DECISION to render and
// push, not merely around the final push -- see bb_data_http_common.c's
// bb_data_http_push_pump() implementation doc for the full per-key
// sequence. In short: the generation compare-and-advance is done under a
// short portMUX critical section BEFORE render_fn runs, so only one caller
// ever wins the right to render+push for a given (key, generation) pair;
// render_fn itself still runs entirely unlocked (it can block); on success,
// COMMIT re-checks the claim is still current (compare-and-discard) before
// pushing -- a slow render_fn returning success AFTER a newer concurrent
// claim has already committed fresher state is discarded, not pushed,
// because render_fn's own contract is "render CURRENT value", not a frozen
// per-generation snapshot, so a stale-but-successful render can easily
// carry content a newer push already delivered; on failure, REVOKE
// symmetrically compare-and-restores the claim, so the event is retried
// later rather than silently lost, UNLESS a second caller has since
// claimed a newer generation for that key, in which case the revoke is a
// no-op and that newer claim's own outcome stands. Both compares (COMMIT's
// and REVOKE's) are what prevent the double-delivery hazard a push-only,
// or claim-only, lock would leave open: without locking the decision
// itself, two concurrent callers could both read the same stale
// last-seen-generation, both render, and both push (the same-generation
// case); without COMMIT's own re-check, a slow claimant's stale-but-
// successful render could still land a second frame AFTER a faster,
// later-claimed concurrent render already pushed (the cross-generation
// case) -- ring entries carry only an attach index, not a generation, so
// neither case has any downstream dedup to fall back on.
//
// What stays outside the lock, and single-task-owned:
//   - render_fn itself (can block; contract requires it stay out of any
//     critical section)
//   - draining the ring into any client's own outbound queue, and the
//     outbound flush/retry state machine -- both stay entirely inside
//     bb_data_http_sweep_step(), unlocked, and single-broadcaster-task-
//     owned exactly as before this PR (see the SCOPE FENCE note above and
//     bb_data_http_client_t's TASK OWNERSHIP doc, bb_data_http_internal.h)
// A second caller must still only ever push EVENT frames through this
// function -- it must never call bb_data_http_sweep_step() itself, nor
// touch any client state directly.
//
// Cross-generation duplication is now CLOSED, not merely reduced: before
// this PR, this loop was single-task and strictly serial, so render+push
// for one generation always completed before the next generation of the
// same key could even be claimed -- same-key cross-generation duplication
// was structurally impossible then, for the mundane reason that no second
// caller existed at all. A second concurrent caller makes it newly
// REACHABLE (see the cross-generation case above), and COMMIT's
// compare-and-discard is what closes it back off: a stale-but-successful
// render for an outdated claim is always discarded, never pushed, once a
// newer claim exists.
//
// ABA note on REVOKE's compare-and-restore: it is reachable for a THIRD
// caller's own earlier revoke to have coincidentally restored
// s_event_last_gen[k] back to exactly the value THIS caller claimed (e.g.
// A claims g1 and stalls; B claims g2, fails, revokes to g1; A later fails
// too, sees g1 -- its own claimed value, coincidentally restored by B's
// unrelated revoke -- and restores to g0). This is provably self-healing,
// not a correctness gap: generations are monotonic and never repeat, so
// any wrongly-reverted marker is exceeded by the very next real generation
// change, which re-triggers a fresh claim for that key. The only cost is a
// redundant re-render of a generation that may already be reflected in a
// later push, never a permanently lost event or a stuck marker --
// deliberately not worth an epoch/token scheme to close. PRECONDITION:
// this self-healing property depends on this function continuing to be
// invoked periodically by every caller sharing this key (the broadcaster's
// existing sweep-tick cadence, and whatever cadence a second caller uses)
// -- without that, this class of transient staleness has no future call
// to heal on and would persist indefinitely instead. A caller wired to
// invoke this function only on-demand (rather than on a recurring tick)
// does not get the self-healing guarantee for free.
//
// SUSTAINED-CHURN NOTE: under continuous generation churn for one key
// faster than either caller's render_fn completes, COMMIT's compare-and-
// discard (above) can keep discarding every render -- throughput for that
// key can drop to zero while churn continues. This is EXPECTED under this
// component's coalesce-to-latest contract (render_fn always renders the
// key's CURRENT value, never a frozen per-generation snapshot -- see
// bb_data_http_render_fn's own doc), not a lock defect: once churn settles,
// the next successful COMMIT reflects the latest state and delivery
// resumes. Do not read sustained zero throughput under heavy churn as
// evidence the lock is broken.
//
// This protocol closes the double-delivery hazard; it has been verified
// by inspection and by a full host+ESP-IDF build only. The host test
// harness is single-threaded (no pthreads), so it cannot reproduce a
// genuine two-task ring race -- that stress exercise is still owed once a
// real second caller exists (B1-1451 wires a producer task to call this
// function; that is the natural place to add it).
void bb_data_http_push_pump(void);

// Diagnostics: cumulative count of render_fn failures observed across every
// sweep_step() call since bb_data_http_init() (or, on host,
// bb_data_http_reset_for_test()). A render_fn that fails leaves the
// offending key's dirty bit set for a retry rather than dropping the update
// (see bb_data_http_sweep_step() above) -- this counter is what makes a
// persistently-failing render_fn observable instead of silently stuck.
size_t bb_data_http_render_fail_count(void);

// Diagnostics: cumulative count of EVENT-kind entries dropped for client `c`
// because its own outbound queue had no room when drained (see
// bb_data_http_sweep_step()'s EVENT drain doc). Cumulative -- never resets
// across sweeps; a "dropped:N" marker frame is queued for the client as soon
// as outbound has room, using this same value. Returns 0 if `c` is NULL.
uint32_t bb_data_http_client_dropped_count(const bb_data_http_client_t *c);

#ifdef BB_DATA_HTTP_TESTING
// Test-only: releases every client, clears the attach table, clears the
// describe table, clears the installed seams, and resets init state to
// uninitialized.
void bb_data_http_reset_for_test(void);
#endif

#ifdef ESP_PLATFORM
// ---------------------------------------------------------------------------
// ESP-IDF composition entry points (B1-1033 first bench-flash de-risk,
// design KB 1447) -- platform/espidf/bb_data_http/bb_data_http_espidf.c.
// Wires render_fn/generation_fn to bb_data_render()/bb_data_generation()
// and send_fn to a raw-socket SSE write, and drives
// bb_data_http_sweep_step() from a single, process-lifetime broadcaster
// task. bb_http_request_t (bb_core.h, already included above) is an opaque
// handle -- no platform type leaks through this declaration.
// ---------------------------------------------------------------------------

// Idempotent: installs the render/generation/send seams and starts the
// broadcaster task (bb_task_create, BB_TASK_BACKING_DYNAMIC) on first call;
// a second call returns BB_OK without creating a second task. Single-caller
// contract: call once from the composition root / lifecycle observer, never
// concurrently from multiple tasks -- the idempotence check (s_started) is
// not itself lock-protected.
// NOTE: there is deliberately no matching stop()/teardown entry point in
// this de-risk -- the example's "http" service is start-once for its
// lifetime. Full lifecycle teardown is deferred to the B1-1045 cutover.
// PREREQUISITE (review fix, B1-1425): bb_data_http_init() must have been
// called first -- this function does NOT call it internally. Skipping it
// is not immediately visible here: the broadcaster task still starts and
// sweeps without error, but every client connect through either
// routes-registering entry point below fails downstream, since
// bb_data_http_client_acquire() (components/bb_data_http/src/
// bb_data_http_common.c) gates on bb_data_http_init() having run and
// returns BB_ERR_INVALID_STATE otherwise.
bb_err_t bb_data_http_espidf_start(void);

// Handles one SSE connect on the calling (httpd) task: sets socket
// hardening (SO_SNDTIMEO/SO_RCVTIMEO/TCP_NODELAY), sends SSE response
// headers plus a ": connected" comment, begins the async handler, acquires
// a bb_data_http client slot with its own SSE send_fn/abort_fn (B1-1448), and
// records the fd -> async request mapping that SSE send_fn and the
// peer-liveness pre-pass use.
// Returns before any data is drained -- draining happens on the next
// broadcaster sweep, never on the calling task. bb_data_http_espidf_start()
// must have been called first (its seams are what the sweep actually
// drains through).
bb_err_t bb_data_http_espidf_client_connect(bb_http_request_t *req,
                                            const char *topic_filter);

// Registers GET /api/events on `server`: the real query-param-parsing
// handler (bb_data_http_espidf_client_connect() wrapper, ?topic= filter)
// plus the OpenAPI route descriptor (bb_data_http_events_route()), via
// bb_http_register_route() + bb_http_register_route_descriptor_only() --
// mirrors bb_diag_routes_init()'s composition-root-called-once pattern
// (B1-1215: previously the composition root wired the handler directly and
// no descriptor was ever registered, so /api/events was absent from
// /api/openapi.json on every board). bb_data_http_espidf_start() should be
// called first so the sweep task is already running when a client connects,
// and bb_data_http_init() before that (see its PREREQUISITE note above) --
// a connect through this route with bb_data_http_init() never called fails
// with BB_ERR_INVALID_STATE (bb_data_http_client_acquire()). A connect
// before any key is attached (bb_data_http_attach()) still succeeds but
// streams nothing for that topic.
// bb_http_handle_t opaque handle (bb_core.h, already included above) -- no
// platform type leaks through this declaration.
bb_err_t bb_data_http_espidf_routes_init(bb_http_handle_t server);

// Registers the WS egress endpoint GET /ws/events on `server` (B1-1050
// PR-1): installs bb_ws_server's process-global connect/disconnect
// callbacks (acquiring/releasing a bb_data_http client with its own WS
// send_fn/abort_fn, B1-1448, per WS session -- see
// bb_ws_server_set_connect_cb/set_disconnect_cb's own
// "one registration per process" contract) and registers the endpoint
// itself via bb_ws_server_register_endpoint(). Inbound DATA frames on this
// endpoint are explicitly discarded (bb_data_http has no recv concept; see
// the platform/espidf/bb_data_http/bb_data_http_espidf.c handler's own
// comment -- WS ingress is a separate, tracked design, epic B1-828).
//
// Topic filtering is NOT available on this path (B1-1423, tracked
// follow-up): every WS client acquired here subscribes to all attached
// topics, unlike GET /api/events' `?topic=` query param -- bb_ws_server's
// connect callback carries no request handle to parse one from.
//
// bb_data_http_espidf_start() should be called first so the sweep task is
// already running when a client connects, and bb_data_http_init() before
// that, same as bb_data_http_espidf_routes_init() above -- see its doc
// (and bb_data_http_espidf_start()'s PREREQUISITE note) for the exact
// failure/degradation each missing prerequisite produces. bb_http_handle_t
// opaque handle (bb_core.h, already included above) -- no platform type
// leaks through this declaration.
bb_err_t bb_data_http_espidf_ws_routes_init(bb_http_handle_t server);

// Convenience wrapper for a SECOND, producer-owned caller (B1-1451, epic
// B1-1123): bb_data_http_push_pump() plus an immediate wake of the
// broadcaster task, so a producer's EVENT push does not wait for the
// broadcaster's own next scheduled sweep to be DRAINED to a connected
// client. Equivalent to:
//
//   bb_data_http_push_pump();
//   <wake the broadcaster task>
//
// PERIODICITY PRECONDITION PRESERVED (bb_data_http_push_pump()'s own
// MULTI-WRITER CONTRACT doc, above): this function's wake is a bounded-
// TIMEOUT nudge on the broadcaster's existing wait, never a replacement for
// its unconditional periodic tick -- broadcaster_task()
// (bb_data_http_espidf.c) still calls bb_data_http_sweep_step() (which
// itself calls bb_data_http_push_pump() for every attached EVENT key) on
// its own BB_DATA_HTTP_SWEEP_INTERVAL_MS cadence regardless of whether any
// notification arrives. A key this function is called for is therefore
// STILL pumped by the broadcaster's own periodic sweep, exactly as before
// this function existed -- calling this function only ever makes delivery
// happen SOONER, it never becomes the sole periodic driver for any key,
// including one this function's own caller also pumps. The self-healing
// ABA-revoke property bb_data_http_push_pump() depends on (see its own
// PRECONDITION note) is unaffected.
//
// Always calls bb_data_http_push_pump() first, synchronously, regardless of
// whether the broadcaster task has been started yet (bb_data_http_
// espidf_start()) -- feeding the shared EVENT ring has no dependency on the
// broadcaster's own task handle. The wake itself is a no-op (the push still
// happens) if the broadcaster task has not started: the ring will simply be
// drained on the broadcaster's first sweep once it does start, same as any
// other push_pump() caller.
//
// Safe to call from any task, including concurrently with the broadcaster
// task itself and/or a second producer task calling this function on a
// different key -- see bb_data_http_push_pump()'s own MULTI-WRITER CONTRACT
// doc for the full concurrency argument; this function adds nothing beyond
// an xTaskNotifyGive()-class wake on top of that already-safe call.
//
// Always returns BB_OK -- degrades gracefully (push still happens, wake is
// skipped) rather than failing when the broadcaster task is not yet
// running, mirroring this component's other setters/no-op-when-missing
// seams.
bb_err_t bb_data_http_notify_push(void);
#endif

#ifdef __cplusplus
}
#endif
