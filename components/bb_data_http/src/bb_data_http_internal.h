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
// event_cursor is reserved for PR-3 (shared EVENT ring + per-client cursor +
// dropped:N, B1-1032) -- present so the struct shape matches the converged
// design now, but unread/unwritten by this PR's sweep-step.
struct bb_data_http_client {
    bool       in_use;
    int        fd;
    bool       is_ws;
    char       topic_filter[BB_DATA_HTTP_TOPIC_MAX];  // "" == all attached keys
    uint32_t   event_cursor;                          // unused until PR-3
    uint32_t   state_dirty_mask;
    uint32_t   state_seen_gen[BB_DATA_HTTP_MAX_ATTACH];
    bb_queue_t outbound;
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
#endif

#ifdef __cplusplus
}
#endif
