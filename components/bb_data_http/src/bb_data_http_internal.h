#pragma once
// Private interface for bb_data_http_common.c and its host test file. Not
// for external consumers -- kept out of include/.
#include "bb_data_http.h"
#include "bb_http.h"
#include "bb_queue.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// STATE (poll) bookkeeping, split out of bb_data_http_client_t (B1-1447,
// epic B1-1123) into its own pool-allocated block -- see
// bb_data_http_common.c's poll pool. dirty_mask / seen_gen are indexed by
// ATTACH-TABLE index (0..BB_DATA_HTTP_MAX_ATTACH-1), NOT by bb_data binding
// index -- this component never sees bb_data binding indices, only
// attach-table slots it owns itself.
typedef struct {
    uint32_t dirty_mask;
    uint32_t seen_gen[BB_DATA_HTTP_MAX_ATTACH];
} bb_data_http_poll_state_t;

// EVENT (push) bookkeeping, split out of bb_data_http_client_t (B1-1447,
// epic B1-1123) into its own pool-allocated block -- see
// bb_data_http_common.c's push pool.
//
// cursor (B1-1033 PR-3, KB 1443/1444) is this client's read position into
// the shared EVENT ring (see bb_data_http_common.c's s_event_ring): the next
// global push-sequence number this client has not yet drained. Set to the
// ring's current total-pushed count on acquire -- a freshly-attached client
// only receives EVENTs pushed AFTER it connects, never ring backlog (mirrors
// an SSE "new events only" subscription rather than replay-on-connect;
// STATE's fresh-render-on-connect semantics do not apply to EVENT).
//
// dropped / drop_marker_pending implement the backpressure contract (see
// bb_data_http_sweep_step()'s EVENT drain doc): when this client's own
// `outbound` queue has no room for a drained event, the event is dropped
// (not evicted from the shared ring -- other clients are unaffected) and
// dropped increments. A "dropped:N" marker frame is queued for this client
// at the next opportunity outbound has room, then drop_marker_pending
// clears; dropped itself is cumulative and never resets
// (bb_data_http_client_dropped_count()).
typedef struct {
    uint32_t cursor;
    uint32_t dropped;
    bool     drop_marker_pending;
} bb_data_http_push_state_t;

// One fd-table client slot (B1-1033 Option A, KB 1443). Indexed 0..N-1 by
// slot position within the static pool -- see bb_data_http_common.c.
//
// poll (B1-1447): STATE bookkeeping, pool-allocated at acquire time only
// when `subscribe_mask` includes BB_DATA_HTTP_SUBSCRIBE_STATE -- NULL
// otherwise (an EVENT-only client never touches it; every call site that
// would dereference it gates on `subscribe_mask` or `poll != NULL` FIRST --
// see bb_data_http_client_acquire()'s force-dirty walk and
// bb_data_http_sweep_step()'s detect/drain phases). A client whose kind the
// core cannot back with a pool slot (pool exhausted) never comes into
// existence -- bb_data_http_client_acquire() fails BB_ERR_NO_SPACE instead
// of returning a client with a NULL poll a caller might not expect (see
// bb_data_http_client_acquire()'s doc, bb_data_http.h).
//
// push (B1-1447): EVENT bookkeeping, pool-allocated at acquire time only
// when `subscribe_mask` includes BB_DATA_HTTP_SUBSCRIBE_EVENT -- NULL
// otherwise, same non-participation discipline as poll above (see
// bb_data_http_sweep_step()'s drain phase, which gates
// drain_client_events() on `push != NULL`).
//
// outbound_max_bytes mirrors the byte budget passed to bb_queue_create()
// for `outbound` at acquire time. bb_queue exposes no "would this push fit"
// query, so the EVENT drain path (which must NOT rely on outbound's
// BB_QUEUE_EVICT_OLDEST auto-eviction -- see the drop-not-evict rationale
// above) keeps its own copy to pre-check room before pushing.
//
// send_fail_count (B1-1424/B1-1429) is the bounded send-retry contract's
// per-client consecutive-failure counter -- see bb_data_http_sweep_step()'s
// flush-contract doc (bb_data_http.h) for the full design. Reset to 0 on
// every successful send_fn call (and on acquire); incremented on every
// RETRIABLE (BB_ERR_TIMEOUT) failure only -- a FATAL failure tears the
// client down immediately instead of touching this counter. It is a 4-byte
// cost per client slot (CONFIG_BB_DATA_HTTP_MAX_CLIENTS * 4 bytes total BSS,
// e.g. 8 bytes at the default cap of 2) -- deliberately per-CLIENT, not
// per-frame: bb_queue's entries carry no spare per-entry field to hang a
// counter off of, and a wedged transport (the failure mode this bounds)
// fails every frame it is asked to send, not just one particular frame, so
// a single client-wide counter observes the same signal a per-frame counter
// would, for a quarter the state and no bb_queue changes.
//
// send_fn/send_ctx/abort_fn/abort_ctx (B1-1123 PR-1) are the PER-CONSUMER
// seam override -- see bb_data_http.h's PER-CONSUMER SEAMS doc for the full
// rationale. NULL (the zero-default -- every existing HTTP call site leaves
// these unset) means "fall back to the module-wide default installed via
// bb_data_http_set_send_fn()/bb_data_http_set_abort_fn()"; the flush loop
// (bb_data_http_common.c) resolves `c->send_fn ? c->send_fn : s_send_fn`
// (and likewise for abort_fn) on every call. Written once by the acquiring
// task at acquire time, then read-only for the rest of this client's
// lifetime by the single task that owns bb_data_http_sweep_step() -- same
// discipline as every other field below except pending_release.
//
// TASK OWNERSHIP (B1-1424 HIGH fix, deferred reap): every field below is
// created by whichever task calls bb_data_http_client_acquire()
// (fully populated, then PUBLISHED via `in_use = true` as the last store --
// see bb_data_http_client_acquire(), bb_data_http_common.c), then
// EXCLUSIVELY owned and mutated by the single task that calls
// bb_data_http_sweep_step() (the espidf backend's broadcaster task; the
// test-runner thread on host) for the rest of the client's lifetime,
// INCLUDING the eventual bb_data_http_client_release() call itself --
// bb_data_http_client_release() is NOT cross-task-safe and must only ever
// be called from that one owning task (see its doc, bb_data_http.h). The
// ONE exception is `pending_release`: any task may SET it (via
// bb_data_http_client_request_release()) to ask the owning task to release
// this client on its next bb_data_http_sweep_step() call, but only the
// owning task ever READS or CLEARS it (bb_data_http_client_release() resets
// it back to false as part of a normal release, and
// bb_data_http_client_acquire() resets it again on reuse). This is why
// pending_release is atomic_bool and every other field is a plain type --
// it is the ONLY field a foreign task ever touches, which is what lets the
// rest of this struct stay lock-free and single-task-owned without the
// core growing a platform mutex of its own. A real hazard this closes: the
// espidf backend's WS disconnect callback fires on bb_ws_server's own
// worker task, not the broadcaster -- calling
// bb_data_http_client_release() directly from there (destroying `outbound`,
// clearing `in_use`) could race the broadcaster's own in-flight read/write
// of this SAME client inside bb_data_http_sweep_step() on another core.
//
// SHARED EVENT RING -- a SEPARATE, real exception (B1-1450, epic B1-1123),
// not part of the per-client single-task-ownership model above: the
// module-level EVENT ring (bb_data_http_common.c's s_event_ring,
// s_event_total_pushed, s_event_last_gen -- NOT any field on this struct)
// is now genuinely multi-writer-safe, guarded by a short portMUX critical
// section (s_event_ring_mux) wrapped around a CLAIM/RENDER/COMMIT-OR-
// REVOKE protocol, not merely around the final push -- the generation
// compare-and-advance that decides whether a key needs a fresh render is
// itself claimed under the lock, before render_fn runs, so only one caller
// ever wins the right to render+push a given (key, generation) pair; a
// failed render revokes the claim via a locked compare-and-restore unless
// a second caller has since claimed a newer generation. See
// bb_data_http_push_pump()'s doc (bb_data_http.h) for the exact per-key
// sequence and why locking only the push (without the decision) would
// still leave a double-delivery hazard open. This lets a second,
// independent producer task call bb_data_http_push_pump() directly,
// concurrently with the broadcaster task's own sweep_step()-driven call.
// It does NOT change anything about this struct or the per-client
// ownership rules above: draining the ring into any client's `outbound`
// (push->cursor, dropped, drop_marker_pending, outbound itself) is still
// exclusively read/written by the single owning task inside
// bb_data_http_sweep_step(), unlocked, exactly as before -- only the
// ring's own module-level statics gained the claim/commit/revoke lock, not
// the per-client drain/flush path.
struct bb_data_http_client {
    bool                            in_use;
    char                            topic_filter[BB_DATA_HTTP_TOPIC_MAX];  // "" == all attached keys
    uint32_t                        subscribe_mask;  // resolved kind bitmask; see bb_data_http_client_cfg_t's doc (bb_data_http.h)
    bb_data_http_poll_state_t      *poll;  // NULL unless subscribe_mask includes STATE -- see its own doc above
    bb_data_http_push_state_t      *push;  // NULL unless subscribe_mask includes EVENT -- see its own doc above
    bb_queue_t                      outbound;
    size_t                          outbound_max_bytes;
    uint32_t                        send_fail_count;
    atomic_bool                     pending_release;
    bb_data_http_send_fn            send_fn;   // NULL -> module-wide default (s_send_fn)
    void                           *send_ctx;
    bb_data_http_abort_fn           abort_fn;  // NULL -> module-wide default (s_abort_fn)
    void                           *abort_ctx;
    bb_data_http_client_lifetime_t  lifetime;  // resolved at acquire; see bb_data_http_client_cfg_t's doc (bb_data_http.h)
};

// Pinned shrink proof (B1-1447, re-pinned B1-1448, re-pinned B1-1465/B1-1466
// for the `lifetime` field above): splitting STATE/EVENT bookkeeping into
// pool-allocated poll/push pointers, and later dropping the last
// HTTP-specific fd/is_ws fields (B1-1448, epic B1-1123), must actually
// SHRINK this struct, not just rename fields around the same footprint --
// see bb_serialize_json_tok.c for this repo's identical pinned-sizeof
// convention. A future field added here (or a future revert) that changes
// this size needs a deliberate, reviewed edit to this assert, not a silent
// drift. Pointer-width-dependent (this struct carries several pointer-sized
// fields): 120 bytes on a 64-bit host build (native test envs), 84 bytes on
// a 32-bit target (ESP32/xtensa ILP32) -- both pinned here rather than
// assuming one ABI, since this header is compiled by both. `lifetime` is a
// single-enumerator-width field (4 bytes) appended at the end of the
// struct, so it grew both ABIs by exactly one pointer-independent word
// (8 bytes on LP64 due to trailing struct-alignment padding to the next
// 8-byte boundary, 4 bytes on ILP32 with no such padding needed) -- from
// 112/80 (B1-1448) to 120/84. PRECONDITION: this two-arm ternary only
// distinguishes ILP32 (4-byte pointers, e.g. ESP32/xtensa) from LP64
// (8-byte pointers, e.g. native host); it does NOT cover a 16-bit-pointer
// target (e.g. an AVR/Arduino backend, which this workspace does have as a
// family, though bb_data_http itself is realistically ESP32/host-only) --
// such a build would spuriously trip this assert and need a third arm
// added, not silently pass under either existing one.
_Static_assert(sizeof(struct bb_data_http_client) == (sizeof(void *) == 8 ? 120 : 84),
               "bb_data_http_client_t size changed -- update this pin (B1-1465/B1-1466 lifetime field)");

#ifdef BB_DATA_HTTP_TESTING
// Test accessors -- expose fd-table/attach-table internals without widening
// the public API surface.

// Returns the dirty-mask bitset currently set on client `c`. Returns 0 if
// `c` is NULL.
uint32_t bb_data_http_client_dirty_mask_for_test(const bb_data_http_client_t *c);

// Returns client `c`'s recorded state_seen_gen for attach index `idx`.
// Returns 0 if `c` is NULL or idx is out of range.
uint32_t bb_data_http_client_seen_gen_for_test(const bb_data_http_client_t *c, size_t idx);

// Returns the number of entries currently queued in client `c`'s outbound
// bb_queue. Returns 0 if `c` is NULL.
size_t bb_data_http_client_outbound_count_for_test(const bb_data_http_client_t *c);

// Returns client `c`'s current consecutive RETRIABLE send_fn failure count
// (see bb_data_http_client_t's send_fail_count doc). Returns 0 if `c` is
// NULL.
uint32_t bb_data_http_client_send_fail_count_for_test(const bb_data_http_client_t *c);

// Returns client `c`'s current event_cursor (next undrained EVENT global
// sequence number). Returns 0 if `c` is NULL.
uint32_t bb_data_http_client_event_cursor_for_test(const bb_data_http_client_t *c);

// Returns client `c`'s current pending_release flag (see
// bb_data_http_client_t's TASK OWNERSHIP doc). Returns false if `c` is
// NULL.
bool bb_data_http_client_pending_release_for_test(const bb_data_http_client_t *c);

// B1-1447: direct proof that a client whose subscribe_mask excludes STATE
// (or the STATE poll pool was exhausted -- see
// bb_data_http_client_acquire()'s doc, bb_data_http.h) carries a genuinely
// NULL `poll` pointer, rather than inferring it indirectly from every
// poll-reading accessor's own NULL-safe zero-return. Returns true if `c` is
// NULL (mirrors "no client, nothing to point at").
bool bb_data_http_client_poll_is_null_for_test(const bb_data_http_client_t *c);

// Mirror of bb_data_http_client_poll_is_null_for_test() for `push`. Returns
// true if `c` is NULL.
bool bb_data_http_client_push_is_null_for_test(const bb_data_http_client_t *c);

// B1-1449: current occupancy of the shared EVENT ring -- lets a test assert
// bb_data_http_push_pump() fed the ring directly, independent of any
// client's own drained cursor. Returns 0 if the ring has not been created
// (bb_data_http_init() not yet called, or bb_data_http_reset_for_test()
// since).
size_t bb_data_http_event_ring_count_for_test(void);

// B1-1450: current value of the module-level, per-attach-index
// s_event_last_gen for `key` -- lets a test observe the claim/commit/
// revoke protocol's outcome directly. Returns 0 if `key` is not attached.
uint32_t bb_data_http_event_last_gen_for_test(const char *key);

// B1-1450: directly overwrite s_event_last_gen for `key`'s attach index,
// bypassing the claim/commit/revoke protocol -- lets a host test simulate
// a second, concurrent bb_data_http_push_pump() caller claiming a NEWER
// generation for the SAME key while THIS caller's own render_fn call is
// still in flight (the compare-and-restore revoke's "leave it alone"
// branch, see bb_data_http_push_pump()'s own doc, bb_data_http.h), which
// the single-threaded host harness cannot otherwise reach. No-op if `key`
// is not attached.
void bb_data_http_event_last_gen_set_for_test(const char *key, uint32_t gen);
#endif

// GET /api/events route descriptor (B1-1215): schema-only (.handler == NULL)
// so the same static bb_route_t is shared verbatim between the ESP-IDF
// composition helper (bb_data_http_espidf_routes_init(), which pairs it with
// the real handler via bb_http_register_route_descriptor_only()) and host
// tests -- one descriptor, no mirror. .handler stays NULL because this
// descriptor lives in the portable bb_data_http_common.c, which host tests
// compile directly, while the real handler (events_get_handler) lives in an
// ESP-IDF-only platform TU; setting .handler here would leak a platform-only
// symbol into a portable file, so the wiring is split between this
// descriptor and bb_data_http_espidf_routes_init()'s separate handler
// registration. content_type is "text/event-stream" with schema == NULL: a
// multiplexed stream over every bb_data-bound topic has no single response
// schema; bb_openapi's oneOf synthesis (build_sse_oneof_fragment()) fills
// the content block from the registered SSE topic schemas instead.
const bb_route_t *bb_data_http_events_route(void);

#ifdef __cplusplus
}
#endif
