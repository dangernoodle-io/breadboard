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

// ---------------------------------------------------------------------------
// TXT pending cache hooks — tests for TA-404 persistent replay semantics
// ---------------------------------------------------------------------------
// The cache is the source of truth for all set_txt calls.  Simulate the
// lifecycle with:
//   bb_mdns_txt_pending_reset_for_test()  — clear cache (call in setUp)
//   bb_mdns_simulate_start_for_test()     — simulate wifi got-ip / mDNS start
//   bb_mdns_simulate_stop_for_test()      — simulate wifi disconnect / teardown
//
// Inspect with:
//   bb_mdns_txt_pending_count()           — entries currently in the cache
//   bb_mdns_txt_pending_get_value(key)    — value for a key, or NULL if absent
//   bb_mdns_txt_replay_count()            — how many times start was called
//   bb_mdns_txt_is_up()                   — whether mDNS is currently "up"
//   bb_mdns_txt_live_set_count()          — set_txt write-throughs while up

// Reset all txt-pending test state (call in setUp()).
void bb_mdns_txt_pending_reset_for_test(void);

// Simulate a wifi-got-ip / mDNS start event; replays the cache.
void bb_mdns_simulate_start_for_test(void);

// Simulate a wifi-disconnect / mDNS teardown event.
void bb_mdns_simulate_stop_for_test(void);

// Inspectors.
int         bb_mdns_txt_pending_count(void);
const char *bb_mdns_txt_pending_get_value(const char *key);  // NULL if not found
int         bb_mdns_txt_replay_count(void);   // cumulative start() calls
bool        bb_mdns_txt_is_up(void);
int         bb_mdns_txt_live_set_count(void); // write-throughs while mDNS up

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
