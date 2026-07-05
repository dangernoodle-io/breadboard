#pragma once

// bb_mdns_cache — presence adapter bridging bb_mdns browse/hello/bye events
// (and a periodic mdns_query_ptr re-query worker) into bb_cache AGE_OUT-policy
// entries, so a "who's on the LAN right now" view is available via the normal
// bb_cache REST/SSE read paths (bb_cache_get_serialized / bb_cache_foreach)
// without a consumer having to track mDNS peer lifecycle itself.
//
// Keying: entries are keyed "<key_prefix><instance_name>" (prefix default
// "miner."). instance_name is bb_mdns's stable browse identity, but the
// bye/on_removed callback does NOT drive a delete -- see the on_bye comment
// in the ESP-IDF glue for why (browse-refresh churn, not a reliable
// goodbye). hostname/ip4/port live in the PAYLOAD (bb_mdns_cache_entry_t)
// for display, not in the key.
//
// Concurrency: no locks of its own -- bb_cache's per-entry lock serializes
// every read/write. Three contexts touch a given key: the bb_mdns dispatch
// task (hello/bye, kept light), the re-query worker task (independent
// dedicated task via bb_timer_worker_periodic_create), and consumer readers
// (bb_cache_foreach/get, already UAF-safe copy-out).

#include "bb_core.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Kconfig bridge (pattern from bb_clock.h / CLAUDE.md) -- C default MUST
// match the Kconfig default.
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_MDNS_CACHE_REQUERY_PERIOD_S
#define BB_MDNS_CACHE_REQUERY_PERIOD_S CONFIG_BB_MDNS_CACHE_REQUERY_PERIOD_S
#endif
#ifdef CONFIG_BB_MDNS_CACHE_QUERY_TIMEOUT_MS
#define BB_MDNS_CACHE_QUERY_TIMEOUT_MS CONFIG_BB_MDNS_CACHE_QUERY_TIMEOUT_MS
#endif
#ifdef CONFIG_BB_MDNS_CACHE_MAX_RESULTS
#define BB_MDNS_CACHE_MAX_RESULTS CONFIG_BB_MDNS_CACHE_MAX_RESULTS
#endif
#ifdef CONFIG_BB_MDNS_CACHE_STALE_AGE_MS
#define BB_MDNS_CACHE_STALE_AGE_MS CONFIG_BB_MDNS_CACHE_STALE_AGE_MS
#endif
#ifdef CONFIG_BB_MDNS_CACHE_EVICT_AGE_MS
#define BB_MDNS_CACHE_EVICT_AGE_MS CONFIG_BB_MDNS_CACHE_EVICT_AGE_MS
#endif
#ifdef CONFIG_BB_MDNS_CACHE_WORKER_STACK
#define BB_MDNS_CACHE_WORKER_STACK CONFIG_BB_MDNS_CACHE_WORKER_STACK
#endif
#ifdef CONFIG_BB_MDNS_CACHE_WORKER_PRIORITY
#define BB_MDNS_CACHE_WORKER_PRIORITY CONFIG_BB_MDNS_CACHE_WORKER_PRIORITY
#endif
#endif /* ESP_PLATFORM */

#ifndef BB_MDNS_CACHE_REQUERY_PERIOD_S
#define BB_MDNS_CACHE_REQUERY_PERIOD_S 30
#endif
#ifndef BB_MDNS_CACHE_QUERY_TIMEOUT_MS
#define BB_MDNS_CACHE_QUERY_TIMEOUT_MS 3000
#endif
#ifndef BB_MDNS_CACHE_MAX_RESULTS
#define BB_MDNS_CACHE_MAX_RESULTS 16
#endif
#ifndef BB_MDNS_CACHE_STALE_AGE_MS
#define BB_MDNS_CACHE_STALE_AGE_MS 0
#endif
#ifndef BB_MDNS_CACHE_EVICT_AGE_MS
#define BB_MDNS_CACHE_EVICT_AGE_MS 90000
#endif
#ifndef BB_MDNS_CACHE_WORKER_STACK
#define BB_MDNS_CACHE_WORKER_STACK 4096
#endif
#ifndef BB_MDNS_CACHE_WORKER_PRIORITY
#define BB_MDNS_CACHE_WORKER_PRIORITY 3
#endif

// ---------------------------------------------------------------------------
// Field sizes (fixed, not Kconfig-tunable -- payload-display fields only).
// ---------------------------------------------------------------------------
#define BB_MDNS_CACHE_HOSTNAME_MAX      32
#define BB_MDNS_CACHE_IP4_MAX           16
#define BB_MDNS_CACHE_INSTANCE_NAME_MAX 32
#define BB_MDNS_CACHE_KEY_MAX           48
#define BB_MDNS_CACHE_KEY_PREFIX_DEFAULT "miner."

#ifdef __cplusplus
extern "C" {
#endif

// Cache payload -- exactly what bb_cache owns per key. ts_ms (last-seen time)
// is owned by bb_cache's envelope, not duplicated here.
typedef struct {
    char     hostname[BB_MDNS_CACHE_HOSTNAME_MAX];
    char     ip4[BB_MDNS_CACHE_IP4_MAX];
    uint16_t port;
} bb_mdns_cache_entry_t;

// Configuration for bb_mdns_cache_start().
//
//   service           -- mDNS service label (e.g. "_miner"), required.
//   proto             -- mDNS proto label (e.g. "_tcp"), required.
//   key_prefix        -- NULL or "" defaults to BB_MDNS_CACHE_KEY_PREFIX_DEFAULT
//                         ("miner.").
//   stale_age_ms      -- 0 uses the Kconfig default (BB_MDNS_CACHE_STALE_AGE_MS).
//   evict_age_ms      -- 0 uses the Kconfig default (BB_MDNS_CACHE_EVICT_AGE_MS).
//   requery_period_ms -- 0 uses the Kconfig default
//                         (BB_MDNS_CACHE_REQUERY_PERIOD_S * 1000).
typedef struct {
    const char *service;
    const char *proto;
    const char *key_prefix;
    uint32_t    stale_age_ms;
    uint32_t    evict_age_ms;
    uint32_t    requery_period_ms;
} bb_mdns_cache_config_t;

#ifdef ESP_PLATFORM

// Subscribe to bb_mdns browse hello/bye for (service, proto) and register a
// periodic mdns_query_ptr re-query worker. Idempotent per (service, proto).
// Returns BB_ERR_INVALID_ARG if cfg, cfg->service, or cfg->proto is NULL.
bb_err_t bb_mdns_cache_start(const bb_mdns_cache_config_t *cfg);

// Stop the browse subscription and the re-query worker. Does NOT delete any
// existing bb_cache entries -- they age out via their configured AGE_OUT
// policy instead of being force-evicted on stop.
bb_err_t bb_mdns_cache_stop(void);

#endif /* ESP_PLATFORM */

// ---------------------------------------------------------------------------
// Test seam -- pure, portable (host + espidf), no locks, no I/O. Called from
// the SAME code path on-device (hello/bye + re-query worker) and in host
// tests.
// ---------------------------------------------------------------------------

// Build a bb_cache key from prefix + instance_name: "<prefix><instance_name>".
// prefix NULL or "" defaults to BB_MDNS_CACHE_KEY_PREFIX_DEFAULT.
// Returns BB_ERR_INVALID_ARG if out is NULL, out_size == 0, instance_name is
// NULL, or instance_name is empty.
// Returns BB_ERR_NO_SPACE if the built key would not fit in out_size (out is
// still NUL-terminated with the truncated key -- truncation-safe, never
// overflows).
// Returns BB_OK on a full, untruncated write.
bb_err_t bb_mdns_cache_build_key(const char *prefix, const char *instance_name,
                                 char *out, size_t out_size);

// Predicate: is this (instance_name, ip4) pair usable for a cache entry?
// Returns false when instance_name is NULL/empty, when ip4 is NULL/empty, or
// when ip4 contains any character outside '0'-'9' and '.' (not a plausible
// dotted-quad). Returns true otherwise.
bool bb_mdns_cache_result_valid(const char *instance_name, const char *ip4);

#ifdef __cplusplus
}
#endif
