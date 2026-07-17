#pragma once

/**
 * @brief Service run-state authority: register named services, track a
 * computed STOPPED/PAUSED/RUNNING state per service, and let independent
 * subsystems assert/clear open-vocabulary pause reasons without stepping on
 * each other. PUSH (observer), PULL (generic emit sink), and POLL (lock-free
 * version counter) delivery, all sourced from one lock-guarded commit.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "bb_core.h"   // bb_err_t
#include "bb_emit.h"   // bb_emit_fn (bb_core)

#ifdef ESP_PLATFORM
#  include "sdkconfig.h"
#endif
#ifndef CONFIG_BB_LIFECYCLE_MAX_REASONS
#define CONFIG_BB_LIFECYCLE_MAX_REASONS 32
#endif

// Number of uint32_t words needed to hold CONFIG_BB_LIFECYCLE_MAX_REASONS
// interned-reason bits. Bridged here (not only in the .c) so a consumer
// sizing its own inhibit-word buffer for bb_lifecycle_inhibit_words() gets
// the same math without pulling in the .c.
#define BB_LIFECYCLE_INHIBIT_WORDS ((CONFIG_BB_LIFECYCLE_MAX_REASONS + 31) / 32)

#ifdef __cplusplus
extern "C" {
#endif

// Fixed-table-index handle: also doubles as the generic emit `id` and the
// per-service version key. BB_LIFECYCLE_SVC_INVALID never aliases a valid
// registered index.
typedef int32_t bb_lifecycle_svc_t;
#define BB_LIFECYCLE_SVC_INVALID ((bb_lifecycle_svc_t)-1)

// Effective, computed state — never stored directly (see bb_lifecycle.c
// compute_state()). STOPPED dominates PAUSED dominates RUNNING.
typedef enum {
    BB_LIFECYCLE_STOPPED = 0,
    BB_LIFECYCLE_PAUSED  = 1,
    BB_LIFECYCLE_RUNNING = 2,
} bb_lifecycle_state_t;

// Reasons are OPEN-VOCABULARY strings owned entirely by consumers —
// bb_lifecycle ships no reason constants of its own. The first distinct
// string a caller passes to bb_lifecycle_pause_assert() interns a new bit
// (globally, shared across every service); a later call with an
// already-interned string reuses that same bit.
#define BB_LIFECYCLE_REASON_NONE ((uint8_t)0xFF)  // payload sentinel: start/stop have no single reason bit

#define BB_LIFECYCLE_EVENT_TOPIC "lifecycle.state"

// Wire/observer payload for one state transition. `inhibits` carries only
// the low 32 interned bits — a complete picture as long as
// CONFIG_BB_LIFECYCLE_MAX_REASONS <= 32 (the shipped default); a consumer
// running with a larger reason table should call
// bb_lifecycle_inhibit_words() for the full picture.
typedef struct {
    int32_t  svc;
    uint32_t inhibits;
    uint32_t version;
    uint8_t  old_state;  // bb_lifecycle_state_t
    uint8_t  new_state;  // bb_lifecycle_state_t
    uint8_t  reason;     // interned bit index that changed, or BB_LIFECYCLE_REASON_NONE
    uint8_t  _pad;
} bb_lifecycle_event_t;

typedef void (*bb_lifecycle_observer_fn)(const bb_lifecycle_event_t *evt, void *user);

// Append-only observer registration. Fixed capacity
// (CONFIG_BB_LIFECYCLE_MAX_OBSERVERS); full -> BB_ERR_NO_SPACE. Observers
// fire synchronously, in registration order, AFTER the state commit and
// OUTSIDE bb_lifecycle's internal lock — an observer may call any query
// function here, but MUST NOT call a mutator (start/stop/pause_assert/
// pause_clear/register) from within its callback.
bb_err_t bb_lifecycle_observe(bb_lifecycle_observer_fn cb, void *user);

// Opt-in ASYNC observer registration (B1-1034): the same append-only table
// as bb_lifecycle_observe(), but `cb` is invoked on a dedicated drain task
// instead of inline on the emitter — decoupling a slow/blocking observer
// from the caller of bb_lifecycle_start()/stop()/pause_assert()/pause_clear().
// The "observer must not call a lifecycle mutator" rule holds for async
// callbacks too. Requires CONFIG_BB_LIFECYCLE_ASYNC=y (default n); otherwise
// always returns BB_ERR_UNSUPPORTED. NULL cb -> BB_ERR_INVALID_ARG. Observer
// table full -> BB_ERR_NO_SPACE. A bb_bqueue_create()/bb_task_create()
// failure on the first-ever call (lazy queue/task init) is propagated as-is.
bb_err_t bb_lifecycle_observe_async(bb_lifecycle_observer_fn cb, void *user);

// bbtool:init tier=early fn=bb_lifecycle_autoinit
bb_err_t bb_lifecycle_autoinit(void);

typedef struct {
    const char *name;  // required; copied (truncated) into the service table
} bb_lifecycle_config_t;

// Register a new service, starting in BB_LIFECYCLE_STOPPED. Duplicate name
// -> BB_ERR_CONFLICT. Table full -> BB_ERR_NO_SPACE. NULL cfg/cfg->name/out
// -> BB_ERR_INVALID_ARG.
bb_err_t bb_lifecycle_register(const bb_lifecycle_config_t *cfg, bb_lifecycle_svc_t *out);

// Look up a previously-registered service by name. Miss -> BB_ERR_NOT_FOUND.
bb_err_t bb_lifecycle_find(const char *name, bb_lifecycle_svc_t *out);

// Registered name for svc, or "" for an invalid/unregistered handle.
const char *bb_lifecycle_name(bb_lifecycle_svc_t svc);

// Enter the started envelope. Idempotent: already-started is a no-op
// success (no notify, no version bump). Bad handle -> BB_ERR_NOT_FOUND.
bb_err_t bb_lifecycle_start(bb_lifecycle_svc_t svc);

// Leave the started envelope and CLEAR all inhibits for svc. Idempotent:
// already-stopped is a no-op success. Bad handle -> BB_ERR_NOT_FOUND.
bb_err_t bb_lifecycle_stop(bb_lifecycle_svc_t svc);

// Assert a pause reason (open-vocabulary string, interned on first use).
// Setting an already-set bit is an idempotent no-op (no notify, no version
// bump). NULL reason -> BB_ERR_INVALID_ARG. Bad handle -> BB_ERR_NOT_FOUND.
// A brand-new reason string when the intern table is full -> BB_ERR_NO_SPACE.
bb_err_t bb_lifecycle_pause_assert(bb_lifecycle_svc_t svc, const char *reason);

// Clear a previously-asserted pause reason. Lookup-only: a reason that was
// never interned, or is already clear, is a no-op success (BB_OK). NULL
// reason -> BB_ERR_INVALID_ARG. Bad handle -> BB_ERR_NOT_FOUND.
bb_err_t bb_lifecycle_pause_clear(bb_lifecycle_svc_t svc, const char *reason);

bool                 bb_lifecycle_is_paused(bb_lifecycle_svc_t svc);
bb_lifecycle_state_t bb_lifecycle_state(bb_lifecycle_svc_t svc);

// Lock-free read of svc's transition counter (bumped once per real
// transition; unaffected by idempotent no-ops).
uint32_t bb_lifecycle_version(bb_lifecycle_svc_t svc);

size_t bb_lifecycle_count(void);

// Copy up to max_words of svc's inhibit bitset into out (word 0 = bits
// 0..31, word 1 = bits 32..63, ...). Returns the number of words actually
// written (0 for a bad handle or max_words==0/out==NULL).
size_t bb_lifecycle_inhibit_words(bb_lifecycle_svc_t svc, uint32_t *out, size_t max_words);

// Reverse lookup: the interned reason string for a bit index, or "" for an
// unused/out-of-range bit.
const char *bb_lifecycle_reason_name(uint8_t bit);

// Register a generic emit sink (bb_emit_fn, bb_core/bb_emit.h) fired on
// BB_LIFECYCLE_EVENT_TOPIC after every real state transition, with
// id=(int32_t)svc and payload=bb_lifecycle_event_t. NULL clears it.
// Single-slot, single-consumer, null-safe: register once at init, before
// any bb_lifecycle_register()/start()/etc. calls, from a single thread.
// bbtool:init tier=early fn=bb_lifecycle_set_emit consumes=emit_sink order=0
void bb_lifecycle_set_emit(bb_emit_fn cb);

#ifdef BB_LIFECYCLE_TESTING
// Clear services, envelopes, inhibits, versions, the reason-intern table,
// observers, and the emit slot. For test isolation between cases.
void bb_lifecycle_reset_for_test(void);

// Direct test access to the pure word-set helpers and compute_state() —
// exposed only under BB_LIFECYCLE_TESTING so bit indices >= the compiled
// CONFIG_BB_LIFECYCLE_MAX_REASONS (e.g. bits 32/33/63) can be exercised
// against a caller-sized words[] array without rebuilding MAX_REASONS.
void bb_lifecycle_word_set_for_test(uint32_t *words, size_t nwords, uint8_t bit);
void bb_lifecycle_word_clear_for_test(uint32_t *words, size_t nwords, uint8_t bit);
bool bb_lifecycle_word_test_for_test(const uint32_t *words, size_t nwords, uint8_t bit);
bb_lifecycle_state_t bb_lifecycle_compute_state_for_test(bool started, const uint32_t *words, size_t nwords);

// Directly invoke every async-flagged observer slot with `evt` -- a
// registry-read-only dispatch, no queue/task involved. Exposed for a
// no-threading unit test of the dispatch step in isolation; the real async
// drain task (bb_lifecycle_async.c) calls the SAME internal helper this
// wraps after its own bb_bqueue_receive().
void bb_lifecycle_async_drain_dispatch_for_test(const bb_lifecycle_event_t *evt);

// Pulls (and dispatches) exactly one event off the shared async queue,
// blocking up to timeout_ms while empty -- the identical per-iteration body
// the real drain task's for(;;) loop runs (with BB_BQUEUE_WAIT_FOREVER
// there); this wrapper takes a caller-supplied timeout instead so a test
// pthread can run it in a bounded, joinable loop against the REAL host
// bb_bqueue backend (host's bb_task_create() is a fake no-thread stub --
// see platform/host/bb_task/bb_task_host.c -- so the production "task"
// never actually drains on host). Returns BB_ERR_NOT_FOUND/BB_ERR_TIMEOUT on
// an empty queue (mirrors bb_bqueue_receive()); BB_ERR_UNSUPPORTED if the
// queue does not exist yet (no bb_lifecycle_observe_async() call has
// succeeded) or CONFIG_BB_LIFECYCLE_ASYNC=n.
bb_err_t bb_lifecycle_async_test_drain_once(uint32_t timeout_ms);

// Monotonic count of events dropped by the shared async queue (never
// enqueued because it was full) since process start -- NOT reset by
// bb_lifecycle_reset_for_test() (the underlying bb_bqueue instance is a
// lazily-created, process-lifetime singleton; MPSC mode has no reset()).
// Tests compare a before/after DELTA rather than an absolute value. Returns
// 0 if the queue does not exist yet or CONFIG_BB_LIFECYCLE_ASYNC=n.
size_t bb_lifecycle_async_test_dropped(void);

// Un-latches the async substrate's once-guard (bb_lifecycle_async.c) and
// resets its lazy-init error/queue-handle state: destroys the shared queue
// if one exists (nulling the handle) and resets the drop-log rate-limit
// timestamp. Lets a test that deliberately forces the first-ever
// bb_lifecycle_observe_async() call to fail (e.g. by pre-exhausting the
// bb_bqueue static pool) restore a clean slate for every later test in the
// binary -- otherwise that single once-guarded failure would replay forever.
// No-op (nothing to reset) when CONFIG_BB_LIFECYCLE_ASYNC=n.
void bb_lifecycle_async_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
