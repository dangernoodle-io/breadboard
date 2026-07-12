#pragma once

// bb_cache -- library-internal glue, NOT an application-facing API.
//
// Declarations here are consumed only by bb_cache's own platform sources
// (platform/espidf/bb_cache/bb_cache_espidf.c) and its sole intended internal
// consumer, bb_cache_reactive (platform/espidf/bb_cache_reactive/
// bb_cache_reactive_espidf.c, which REQUIRES bb_cache and therefore has this
// header on its include path). Application code must never include this
// header directly -- use bb_cache.h.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-way notify slot fired by bb_cache whenever a key is actually freed --
// by an explicit bb_cache_delete() or by the AGE_OUT eviction path (LAZY or
// SWEEP) -- AFTER the slot has been freed and marked reusable, outside all
// bb_cache locks. Exactly one consumer may install a hook (bb_cache_reactive
// is the sole intended consumer, wiring its fire_on_remove here so explicit
// deletes and age-out evictions both fire on_remove through the same path).
// Passing NULL uninstalls the hook.
void bb_cache_set_evict_notify_fn(void (*fn)(const char *key));

// One-way notify fired AFTER a successful owned write, OUTSIDE all locks.
// Exactly one composer installs a hook (bb_cache_reactive, future). NULL
// uninstalls. Composition-wired setter -- NOT self-register/AUTOREGISTER.
typedef void (*bb_cache_write_notify_fn)(const char *key, uint32_t version);
void bb_cache_set_write_notify_fn(bb_cache_write_notify_fn fn);

#ifdef __cplusplus
}
#endif
