#pragma once

// bb_cache -- library-internal glue, NOT an application-facing API.
//
// Declarations here are consumed only by bb_cache's own platform sources
// (platform/espidf/bb_cache/bb_cache_espidf.c) and whichever single
// component REQUIRES bb_cache and installs a hook below (currently
// bb_mdns_cache -- see platform/espidf/bb_mdns_cache/bb_mdns_cache.c). That
// component is the current installer, not a privileged or permanent owner.
// Application code must never include this header directly -- use
// bb_cache.h.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-way notify slot fired by bb_cache whenever a key is actually freed --
// by an explicit bb_cache_delete() or by the AGE_OUT eviction path (LAZY or
// SWEEP) -- AFTER the slot has been freed and marked reusable, outside all
// bb_cache locks.
//
// Contract: exactly one COMPOSER installs a hook here -- this stays a single
// function-pointer slot, deliberately NOT an N-observer registry. If more
// than one consumer ever needs eviction notification, the fan-out belongs in
// the application/composition root (one hook installed here, fanned out from
// there), never inside bb_cache itself -- an N-observer registry living
// inside the store is exactly the bb_pub fan-out shape the DI legacy fence
// forbids, and it is exactly what deleting bb_cache_reactive removed.
// Passing NULL uninstalls the hook.
void bb_cache_set_evict_notify_fn(void (*fn)(const char *key));

// One-way notify fired AFTER a successful owned write, OUTSIDE all locks.
// Same single-slot contract as bb_cache_set_evict_notify_fn() above --
// exactly one composer installs a hook; fan-out to multiple consumers is an
// application/composition-root concern, not bb_cache's. NULL uninstalls.
// Composition-wired setter -- NOT self-register/AUTOREGISTER.
typedef void (*bb_cache_write_notify_fn)(const char *key, uint32_t version);
void bb_cache_set_write_notify_fn(bb_cache_write_notify_fn fn);

#ifdef __cplusplus
}
#endif
