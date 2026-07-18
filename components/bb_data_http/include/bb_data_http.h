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
 * THIS PR (B1-1033 PR-2) ships the host-testable pure core + STATE replay
 * path only. No FreeRTOS task, no httpd, no real sockets -- the espidf
 * backend that drives bb_data_http_sweep_step() from a broadcaster task is a
 * later, HW-gated PR (B1-1045). EVENT replay (a shared bb_queue ring +
 * per-client cursor + dropped:N, B1-1032) is deferred to PR-3; this PR only
 * reserves the fd-table's event_cursor field.
 *
 * Composition-root-owned attach table: bb_data_http_attach() maps a bb_data
 * key to a topic name. bb_data itself stays topic-agnostic -- the key<->topic
 * mapping lives entirely here. A client's topic_filter (see
 * bb_data_http_client_acquire_ex) selects which attached keys it receives:
 * NULL/"" subscribes to every attached key; a non-empty filter subscribes
 * only to keys attached under that exact topic name.
 *
 * HARD INVARIANT (the STATE lost-wakeup guarantee, KB 1443): the sweep-step's
 * detect phase writes every dirty client's state_seen_gen to the generation
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
// Replay kind (B1-1032) -- STATE is fully implemented by this PR; EVENT is a
// deliberate no-op stub (deferred to PR-3, the shared ring + cursor design).
// ---------------------------------------------------------------------------
typedef enum {
    BB_DATA_HTTP_STATE = 0,
    BB_DATA_HTTP_EVENT = 1,
} bb_data_http_replay_kind_t;

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
typedef bb_err_t (*bb_data_http_generation_fn)(const char *key, uint32_t *out_gen, void *ctx);

// Sends `len` already-rendered bytes to client `fd`. `is_ws` selects
// WS-framed vs raw SSE-frame delivery -- the send_fn owns actual framing.
// `ctx` is the opaque pointer passed to bb_data_http_set_send_fn(). The real
// backend wraps a socket write (or bb_ws_server send); host tests supply a
// capture stub (platform/host/bb_data_http/bb_data_http_host.h).
typedef bb_err_t (*bb_data_http_send_fn)(int fd, bool is_ws, const void *bytes,
                                          size_t len, void *ctx);

// Install/replace the render seam. Passing fn=NULL disables rendering (dirty
// keys are silently skipped -- see bb_data_http_sweep_step()).
void bb_data_http_set_render_fn(bb_data_http_render_fn fn, void *ctx);

// Install/replace the generation-read seam. Passing fn=NULL disables STATE
// dirty-detection entirely (no key is ever marked dirty).
void bb_data_http_set_generation_fn(bb_data_http_generation_fn fn, void *ctx);

// Install/replace the send seam. Passing fn=NULL disables draining (rendered
// bytes accumulate in the per-client outbound queue until a send_fn is set).
void bb_data_http_set_send_fn(bb_data_http_send_fn fn, void *ctx);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

typedef struct {
    size_t max_clients;  // 0 -> CONFIG_BB_DATA_HTTP_MAX_CLIENTS
} bb_data_http_cfg_t;

// Initialize the transport core. Idempotent; a second call returns BB_OK.
// cfg=NULL uses Kconfig defaults.
bb_err_t bb_data_http_init(const bb_data_http_cfg_t *cfg);

// ---------------------------------------------------------------------------
// Attach table (composition-root owned)
// ---------------------------------------------------------------------------

// Attach `key` (a bb_data binding key) under `topic`, with explicit replay
// kind. Idempotent per key -- re-attaching an already-attached key updates
// its topic/kind in place.
//
// Returns BB_ERR_INVALID_ARG if `key` or `topic` is NULL/empty, or either
// exceeds its buffer bound (BB_DATA_HTTP_KEY_MAX / BB_DATA_HTTP_TOPIC_MAX).
// Returns BB_ERR_NO_SPACE if the attach table is full
// (BB_DATA_HTTP_MAX_ATTACH distinct keys already attached) and `key` is not
// already attached.
bb_err_t bb_data_http_attach_ex(const char *key, const char *topic,
                                bb_data_http_replay_kind_t kind);

// Convenience wrapper: bb_data_http_attach_ex(key, topic, BB_DATA_HTTP_STATE).
bb_err_t bb_data_http_attach(const char *key, const char *topic);

// Diagnostics: number of keys currently attached.
size_t bb_data_http_attach_count(void);

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------

// Acquire a client slot for socket `fd`. `topic_filter` selects which
// attached keys this client receives: NULL/"" subscribes to every attached
// key; a non-empty filter subscribes only to keys attached under that exact
// topic name (bb_data_http_attach's `topic` argument). `is_ws` selects the
// framing send_fn applies. Every STATE key the client is subscribed to is
// marked dirty immediately (fresh-render-on-connect) so the first
// bb_data_http_sweep_step() call after acquire renders and sends it.
//
// Returns BB_ERR_INVALID_ARG if `out` is NULL, or `topic_filter` is
// non-NULL/non-empty and its length (excluding NUL) is >=
// BB_DATA_HTTP_TOPIC_MAX -- mirrors bb_data_http_attach_ex()'s own topic
// bound so an over-length filter is rejected rather than silently truncated
// (a truncated filter can mis-subscribe a client to the wrong topic).
// Returns BB_ERR_INVALID_STATE if bb_data_http_init() has not been called.
// Returns BB_ERR_NO_SPACE if every client slot is in use.
bb_err_t bb_data_http_client_acquire_ex(bb_data_http_client_t **out, int fd,
                                        const char *topic_filter, bool is_ws);

// Convenience wrapper: bb_data_http_client_acquire_ex(out, fd, NULL, is_ws).
bb_err_t bb_data_http_client_acquire(bb_data_http_client_t **out, int fd, bool is_ws);

// Release a client slot: destroys its outbound queue and frees the slot.
// Safe to call with NULL (no-op).
void bb_data_http_client_release(bb_data_http_client_t *c);

// Diagnostics: number of client slots currently in use.
size_t bb_data_http_active_client_count(void);

// ---------------------------------------------------------------------------
// Sweep (pure, synchronous, no task/blocking -- caller drives the cadence)
// ---------------------------------------------------------------------------

// One sweep pass over every attached STATE key and every active client:
//
//   1. Detect: for each attached STATE key, read its current generation
//      (via the injected generation_fn) and compare it against each
//      matching client's own per-key state_seen_gen. On a mismatch, mark
//      that client's dirty bit for the key AND record the observed
//      generation into state_seen_gen -- BEFORE any rendering happens this
//      pass. This is what makes the ordering invariant below race-safe: a
//      generation bump that happens *during* this pass's own render call
//      (below) is invisible to this detect step (state_seen_gen already
//      captured the pre-render value), so it is correctly re-detected on
//      the NEXT sweep_step() call rather than silently absorbed.
//   2. Drain: for each client, for each dirty bit (in ascending attach-index
//      order): call render_fn, and clear the bit ONLY if render_fn succeeds,
//      THEN push the rendered bytes into that client's outbound bb_queue. A
//      missing render_fn (see the setter above) clears the bit unconditionally
//      -- there is nothing to retry. A render_fn that returns an error leaves
//      the bit set so the key is retried on the next sweep_step() call rather
//      than silently dropped; each such failure increments the counter read
//      by bb_data_http_render_fail_count() and is logged at a rate-limited
//      cadence. Once every dirty key has been drained into the queue, the
//      queue is flushed via send_fn.
//
// EVENT-kind attached keys are skipped entirely by this PR (TODO PR-3: shared
// bb_queue ring + per-client cursor + dropped:N, per KB 1442).
//
// A missing render_fn/generation_fn/send_fn degrades gracefully (see the
// setters above) rather than crashing -- useful for host tests exercising
// only one half of the pipeline at a time.
void bb_data_http_sweep_step(void);

// Diagnostics: cumulative count of render_fn failures observed across every
// sweep_step() call since bb_data_http_init() (or, on host,
// bb_data_http_reset_for_test()). A render_fn that fails leaves the
// offending key's dirty bit set for a retry rather than dropping the update
// (see bb_data_http_sweep_step() above) -- this counter is what makes a
// persistently-failing render_fn observable instead of silently stuck.
size_t bb_data_http_render_fail_count(void);

#ifdef BB_DATA_HTTP_TESTING
// Test-only: releases every client, clears the attach table, clears the
// installed seams, and resets init state to uninitialized.
void bb_data_http_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
