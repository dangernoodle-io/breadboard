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

// One fd-table client slot (B1-1033 Option A, KB 1443). Indexed 0..N-1 by
// slot position within the static pool -- see bb_data_http_common.c.
//
// state_dirty_mask / state_seen_gen are indexed by ATTACH-TABLE index (0..
// BB_DATA_HTTP_MAX_ATTACH-1), NOT by bb_data binding index -- this component
// never sees bb_data binding indices, only attach-table slots it owns
// itself.
//
// event_cursor (B1-1033 PR-3, KB 1443/1444) is this client's read position
// into the shared EVENT ring (see bb_data_http_common.c's s_event_ring): the
// next global push-sequence number this client has not yet drained. Set to
// the ring's current total-pushed count on acquire -- a freshly-attached
// client only receives EVENTs pushed AFTER it connects, never ring backlog
// (mirrors an SSE "new events only" subscription rather than replay-on-
// connect; STATE's fresh-render-on-connect semantics do not apply to EVENT).
//
// event_dropped / event_drop_marker_pending implement the backpressure
// contract (see bb_data_http_sweep_step()'s EVENT drain doc): when this
// client's own `outbound` queue has no room for a drained event, the event
// is dropped (not evicted from the shared ring -- other clients are
// unaffected) and event_dropped increments. A "dropped:N" marker frame is
// queued for this client at the next opportunity outbound has room, then
// event_drop_marker_pending clears; event_dropped itself is cumulative and
// never resets (bb_data_http_client_dropped_count()).
//
// outbound_max_bytes mirrors the byte budget passed to bb_queue_create_ex()
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
// TASK OWNERSHIP (B1-1424 HIGH fix, deferred reap): every field below is
// created by whichever task calls bb_data_http_client_acquire[_ex]()
// (fully populated, then PUBLISHED via `in_use = true` as the last store --
// see bb_data_http_client_acquire_ex(), bb_data_http_common.c), then
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
// bb_data_http_client_acquire_ex() resets it again on reuse). This is why
// pending_release is atomic_bool and every other field is a plain type --
// it is the ONLY field a foreign task ever touches, which is what lets the
// rest of this struct stay lock-free and single-task-owned without the
// core growing a platform mutex of its own. A real hazard this closes: the
// espidf backend's WS disconnect callback fires on bb_ws_server's own
// worker task, not the broadcaster -- calling
// bb_data_http_client_release() directly from there (destroying `outbound`,
// clearing `in_use`) could race the broadcaster's own in-flight read/write
// of this SAME client inside bb_data_http_sweep_step() on another core.
struct bb_data_http_client {
    bool         in_use;
    int          fd;
    bool         is_ws;
    char         topic_filter[BB_DATA_HTTP_TOPIC_MAX];  // "" == all attached keys
    uint32_t     event_cursor;
    uint32_t     event_dropped;
    bool         event_drop_marker_pending;
    uint32_t     state_dirty_mask;
    uint32_t     state_seen_gen[BB_DATA_HTTP_MAX_ATTACH];
    bb_queue_t   outbound;
    size_t       outbound_max_bytes;
    uint32_t     send_fail_count;
    atomic_bool  pending_release;
};

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

// Returns client `c`'s is_ws flag, as recorded by
// bb_data_http_client_acquire_ex()'s `is_ws` argument (B1-1050 PR-1: WS
// egress wiring -- the espidf backend's WS acquire/release path relies on
// this flag being correctly threaded through by the pure core, so it needs
// a host-testable seam of its own). Returns false if `c` is NULL.
bool bb_data_http_client_is_ws_for_test(const bb_data_http_client_t *c);
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
