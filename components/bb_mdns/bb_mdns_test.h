#pragma once
// Test-only hooks for bb_mdns coalescing logic.  Gated on BB_MDNS_TESTING so
// production builds never compile against them.  Tests include this header
// instead of reaching into private implementation files.
//
// On host, the coalescing batch is a plain C shadow of the ESP-IDF batch
// (no mutexes, no esp_timer).  Drive it with:
//   bb_mdns_coalesce_reset_for_test()
//   bb_mdns_coalesce_append_for_test(svc, proto, peer, is_removal)
//   bb_mdns_coalesce_flush_for_test()   — simulates the 50 ms timer firing
//
// Then assert via:
//   bb_mdns_coalesce_batch_count()         — entries in the current batch
//   bb_mdns_coalesce_flush_count()         — how many flushes occurred
//   bb_mdns_coalesce_queue_enqueue_count() — queue items posted per flush

#ifdef BB_MDNS_TESTING

#include "bb_mdns.h"
#include <stdbool.h>

// Reset all coalesce test state (call in setUp()).
void bb_mdns_coalesce_reset_for_test(void);

// Inject a synthetic peer (or removal) event into the pending batch.
// Returns BB_ERR_NO_SPACE if the batch is full.
bb_err_t bb_mdns_coalesce_append_for_test(const char *service, const char *proto,
                                          const bb_mdns_peer_t *peer, bool is_removal);

// Simulate the flush timer firing: dispatches all pending batch entries
// via the registered callbacks (one queue enqueue per call) and clears the
// batch.  Returns the number of entries dispatched.
int bb_mdns_coalesce_flush_for_test(void);

// Inspectors.
int bb_mdns_coalesce_batch_count(void);
int bb_mdns_coalesce_flush_count(void);
int bb_mdns_coalesce_queue_enqueue_count(void);
int bb_mdns_coalesce_drop_count(void);

// Simulate a bounded dispatcher queue.  cap=0 means unlimited (default).
// After setting a cap, drive appends until enqueues are refused; then call
// bb_mdns_coalesce_queue_drain_for_test() to free all slots and allow new enqueues.
void bb_mdns_coalesce_queue_depth_cap_set_for_test(int cap);
// Pin the simulated queue depth to n (forces depth >= cap to block flushes).
void bb_mdns_coalesce_queue_depth_hold_for_test(int depth);
void bb_mdns_coalesce_queue_drain_for_test(void);

#endif /* BB_MDNS_TESTING */
