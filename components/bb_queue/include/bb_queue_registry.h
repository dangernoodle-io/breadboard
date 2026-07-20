#pragma once

// bb_queue_registry — thin consumer of the generic bb_registry primitive that
// tracks every live bb_queue_t under a diagnostic name, for surfacing at
// GET /api/diag/rings.
//
// bb_queue_create() self-registers on success; bb_queue_destroy() deregisters
// before freeing. Both are best-effort: a full registry (BB_QUEUE_REGISTRY_MAX)
// or a duplicate name is logged and does NOT fail ring creation/destruction.
//
// This header forward-declares its own bb_queue_t (matching bb_queue.h's
// `typedef struct bb_queue *bb_queue_t;` exactly — legal to redeclare an
// identical typedef in C11) instead of including bb_queue.h. bb_queue_registry.c
// is compiled as part of the bb_queue component alongside bb_queue.c, so no
// cross-component dependency is introduced by this split.

#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Kconfig bridge: honour CONFIG_BB_QUEUE_REGISTRY_MAX from build flags;
// default 16. Single canonical home for this bridge — reused by
// bb_queue_registry.c (sizes the actual registry storage) and by
// bb_ring_diag.h (sizes its host-testable fill snapshot); do not
// re-derive it elsewhere.
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_QUEUE_REGISTRY_MAX
#define BB_QUEUE_REGISTRY_MAX CONFIG_BB_QUEUE_REGISTRY_MAX
#endif
#ifndef BB_QUEUE_REGISTRY_MAX
#define BB_QUEUE_REGISTRY_MAX 16
#endif

// Canonical definition lives in bb_queue.h (`typedef struct bb_queue *bb_queue_t;`)
// — keep this forward declaration in sync if that ever changes.
typedef struct bb_queue *bb_queue_t;

// Register a ring under `name`. Best-effort — see file header.
// Returns BB_ERR_INVALID_ARG if name or r is NULL.
// Returns BB_ERR_NO_SPACE if the registry is full.
// Returns BB_ERR_INVALID_STATE on a duplicate name.
bb_err_t bb_queue_registry_register(const char *name, bb_queue_t r);

// Deregister a previously-registered ring by value. Resolves the ring's
// registered name internally (scans the registry for a matching value) so
// callers do not need to track the name separately.
// Returns BB_ERR_INVALID_ARG if r is NULL.
// Returns BB_ERR_NOT_FOUND if r was never registered (e.g. registration
// failed at creation time due to a full registry).
bb_err_t bb_queue_registry_deregister(bb_queue_t r);

// Current registered ring count.
uint16_t bb_queue_registry_count(void);

// Iterate all registered rings. UNLIKE the underlying bb_registry_foreach,
// this holds bb_queue_registry's own internal lock across the ENTIRE call,
// including every invocation of cb — this is deliberate: it prevents a
// concurrent bb_queue_destroy() from freeing a ring while a diag reader is
// still touching it mid-iteration (use-after-free otherwise).
//
// Foreach contract — cb MUST be:
//   - bounded (finite, fast — it runs while the registry is fully locked)
//   - allocation-free
//   - a pure SNAPSHOT/copy-out: copy the name + whatever scalar stats you
//     need out of `r` into caller-owned storage and return — do not retain
//     `r` or touch it again after cb returns (bb_queue_destroy() may free it
//     the instant this call unlocks)
//   - MUST NOT perform I/O of any kind (no httpd_resp_send_chunk, no
//     network/socket/flash writes, no logging on the hot path) — I/O runs
//     unpredictably long, and holding this lock during I/O stalls every
//     other bb_queue_create/destroy/count/register/deregister caller
//     fleet-wide for the duration. Do all I/O AFTER bb_queue_registry_foreach
//     returns, from the snapshot you copied out.
//   - non-blocking otherwise too (no other locks, no waiting)
//   - must NOT call back into any bb_queue_registry_* function (register,
//     deregister, foreach, count, test_reset) — the lock is not recursive
//     and doing so self-deadlocks.
//
// Reference implementation of this pattern: bb_ring_diag.c's
// rings_get_handler — snapshot phase (foreach copies into a stack array)
// then a separate stream phase (JSON emission, all httpd I/O) after the
// lock is released.
typedef void (*bb_queue_registry_cb_t)(const char *name, bb_queue_t r, void *ctx);
void bb_queue_registry_foreach(bb_queue_registry_cb_t cb, void *ctx);

#ifdef BB_QUEUE_REGISTRY_TESTING
// Reset the registry to its initial (empty) state. Test teardown only.
void bb_queue_registry_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
