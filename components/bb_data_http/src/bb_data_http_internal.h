#pragma once
// Private interface for bb_data_http_common.c and its host test file. Not
// for external consumers -- kept out of include/.
#include "bb_data_http.h"
#include "bb_queue.h"

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
struct bb_data_http_client {
    bool       in_use;
    int        fd;
    bool       is_ws;
    char       topic_filter[BB_DATA_HTTP_TOPIC_MAX];  // "" == all attached keys
    uint32_t   event_cursor;
    uint32_t   event_dropped;
    bool       event_drop_marker_pending;
    uint32_t   state_dirty_mask;
    uint32_t   state_seen_gen[BB_DATA_HTTP_MAX_ATTACH];
    bb_queue_t outbound;
    size_t     outbound_max_bytes;
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

// Returns client `c`'s current event_cursor (next undrained EVENT global
// sequence number). Returns 0 if `c` is NULL.
uint32_t bb_data_http_client_event_cursor_for_test(const bb_data_http_client_t *c);
#endif

#ifdef __cplusplus
}
#endif
