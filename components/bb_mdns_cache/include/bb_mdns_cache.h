#pragma once

// bb_mdns_cache — presence adapter bridging bb_mdns browse/hello/bye events
// (and a periodic mdns_query_ptr re-query worker) into bb_cache AGE_OUT-policy
// entries, so a "who's on the LAN right now" view is available via the normal
// bb_cache read paths (bb_cache_get_raw / bb_cache_foreach) without a
// consumer having to track mDNS peer lifecycle itself.
//
// Keying: entries are keyed "<key_prefix><instance_name>" (prefix default
// "miner."). instance_name is bb_mdns's stable browse identity, but the
// bye/on_removed callback does NOT drive a delete -- on_removed fires on a
// genuine ttl==0 goodbye (reliable), but eviction rides the single AGE_OUT
// authority (bb_cache ts_ms age-out) rather than deletion-on-goodbye. That
// model was never architecturally adopted — not because goodbye is unreliable.
// hostname/ip4/port live in the PAYLOAD (bb_mdns_cache_entry_t)
// for display, not in the key.
//
// Concurrency: no locks of its own -- bb_cache's per-entry lock serializes
// every read/write. Three contexts touch a given key: the bb_mdns dispatch
// task (hello/bye, kept light), the re-query worker task (independent
// dedicated task via bb_timer_worker_periodic_create), and consumer readers
// (bb_cache_foreach/get, already UAF-safe copy-out).

#include "bb_core.h"
#include "bb_mdns.h"
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
#ifdef CONFIG_BB_MDNS_CACHE_ENTRY_MAX
#define BB_MDNS_CACHE_ENTRY_MAX CONFIG_BB_MDNS_CACHE_ENTRY_MAX
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
#ifndef BB_MDNS_CACHE_ENTRY_MAX
#define BB_MDNS_CACHE_ENTRY_MAX 192
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

// Descriptor for a single consumer TXT field to capture, const/rodata --
// dest_offset/dest_len target a slot in the CONSUMER's own struct (NOT
// bb_mdns_cache_entry_t). A consumer builds a `static const
// bb_mdns_txt_field_t[]` table with offsetof()/sizeof() into its own struct
// and hands it to bb_mdns_cache_config_t.txt_fields -- bb_mdns_cache itself
// hardcodes no TXT keys.
typedef struct {
    const char *txt_key;      // TXT key to match, e.g. "board"
    size_t      dest_offset;  // offsetof(consumer_struct_t, field)
    size_t      dest_len;     // sizeof that field
} bb_mdns_txt_field_t;

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
//   entry_size        -- 0 (default) uses bb_mdns_cache_entry_t (today's
//                         identity-only shape, byte-identical back-compat
//                         behavior). >0 stores entry_size bytes/entry in a
//                         CONSUMER-defined struct -- that struct MUST hold
//                         identity (hostname/ip4/port) at the SAME leading
//                         layout as bb_mdns_cache_entry_t. Validated against
//                         BB_MDNS_CACHE_ENTRY_MAX at start() -- see below.
//   txt_fields        -- NULL (default) captures identity only. Non-NULL
//                         points at a const descriptor table (rodata) naming
//                         which TXT keys to capture and where in the
//                         consumer's entry struct to land them. Setting this
//                         REQUIRES entry_size > 0 (a descriptor implies a
//                         consumer struct) -- see bb_mdns_cache_start()'s
//                         validation.
//   txt_count         -- number of entries in txt_fields.
typedef struct {
    const char *service;
    const char *proto;
    const char *key_prefix;
    uint32_t    stale_age_ms;
    uint32_t    evict_age_ms;
    uint32_t    requery_period_ms;
    size_t      entry_size;
    const bb_mdns_txt_field_t *txt_fields;
    size_t      txt_count;
} bb_mdns_cache_config_t;

#ifdef ESP_PLATFORM

// Subscribe to bb_mdns browse hello/bye for (service, proto) and register a
// periodic mdns_query_ptr re-query worker. Idempotent per (service, proto).
// Returns BB_ERR_INVALID_ARG if cfg, cfg->service, or cfg->proto is NULL.
// Returns BB_ERR_INVALID_ARG if cfg->txt_fields/txt_count are set but
// cfg->entry_size == 0 (a descriptor implies a consumer struct).
// Returns BB_ERR_INVALID_ARG if the effective entry_size (cfg->entry_size,
// or sizeof(bb_mdns_cache_entry_t) when 0) exceeds BB_MDNS_CACHE_ENTRY_MAX
// -- fails loud rather than risking a silent stack overflow on the per-entry
// scratch buffer. Both checks are delegated to the pure, host-testable
// bb_mdns_cache_validate_config() below.
bb_err_t bb_mdns_cache_start(const bb_mdns_cache_config_t *cfg);

// Stop the browse subscription and the re-query worker. Does NOT delete any
// existing bb_cache entries -- they age out via their configured AGE_OUT
// policy instead of being force-evicted on stop.
bb_err_t bb_mdns_cache_stop(void);

// Optional self-start using CONFIG_BB_MDNS_CACHE_AUTO_SERVICE /
// CONFIG_BB_MDNS_CACHE_AUTO_PROTO as the compiled-in service/proto. No-op
// (returns BB_OK) unless CONFIG_BB_MDNS_CACHE_AUTOREGISTER is set.
// bbtool:init tier=pre_http fn=bb_mdns_cache_init
bb_err_t bb_mdns_cache_init(void);

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

// Validate a bb_mdns_cache_config_t's entry-size/TXT-descriptor combination
// -- the pure predicate bb_mdns_cache_start() calls before touching any
// state. Returns BB_ERR_INVALID_ARG when txt_fields is non-NULL and
// txt_count > 0 but entry_size == 0 (a descriptor implies a consumer
// struct). Returns BB_ERR_INVALID_ARG when the effective entry_size
// (entry_size, or sizeof(bb_mdns_cache_entry_t) when entry_size == 0)
// exceeds entry_max. Returns BB_OK otherwise.
bb_err_t bb_mdns_cache_validate_config(size_t entry_size,
                                       const bb_mdns_txt_field_t *txt_fields,
                                       size_t txt_count, size_t entry_max);

// Predicate: is this (instance_name, ip4) pair usable for a cache entry?
// Returns false when instance_name is NULL/empty, when ip4 is NULL/empty, or
// when ip4 contains any character outside '0'-'9' and '.' (not a plausible
// dotted-quad). Returns true otherwise.
bool bb_mdns_cache_result_valid(const char *instance_name, const char *ip4);

// Apply a TXT descriptor to a consumer entry buffer -- pure byte copy, no
// locks/clock/I/O. For each descriptor field, finds the matching TXT key in
// txt[] (first match in txt[] order wins on duplicate keys) and copies its
// value into entry+dest_offset, bounded by dest_len and ALWAYS NUL-
// terminated within dest_len (bb_strlcpy semantics -- truncates safely,
// never overflows). Unlisted TXT keys are skipped. A field whose
// [dest_offset, dest_offset+dest_len) range would exceed entry_size is
// skipped (defensive bounds check -- bb_mdns_cache_start() also validates
// the overall entry_size at registration time, this is a second guard on
// the per-field range).
// No-op when entry is NULL, entry_size == 0, fields is NULL, field_count ==
// 0, txt is NULL, or txt_count == 0.
void bb_mdns_cache_apply_txt(void *entry, size_t entry_size,
                              const bb_mdns_txt_field_t *fields, size_t field_count,
                              const bb_mdns_txt_t *txt, size_t txt_count);

// bb_mdns_cache_txt_serialize (the bb_json_t mirror of the walk above) is
// DELETED (B1-1149) -- its only production caller, entry_serialize()
// (platform/espidf/bb_mdns_cache/bb_mdns_cache.c), was itself deleted in
// B1-1146b, leaving only a host test as caller. The same walk now lives, on
// the wire-descriptor path, in bb_mdns_cache_entry_wire_fill() (components/
// bb_mdns_cache/bb_mdns_cache_wire_priv.h / bb_mdns_cache_wire.c).

// Build-time contract check for a consumer's TXT-capture entry struct: MUST
// hold identity (hostname/ip4/port) at the SAME leading layout as
// bb_mdns_cache_entry_t (see bb_mdns_cache_config_t.entry_size doc above).
// Place one invocation at file scope in the consumer's own source (NOT
// inside bb_mdns_cache) to turn a layout drift into a build error instead of
// a silent runtime corruption of the identity fields.
#define BB_MDNS_CACHE_ASSERT_IDENTITY_LAYOUT(consumer_struct_t)              \
    _Static_assert(offsetof(consumer_struct_t, hostname) ==                 \
                       offsetof(bb_mdns_cache_entry_t, hostname),            \
                   #consumer_struct_t ".hostname must match bb_mdns_cache_entry_t layout"); \
    _Static_assert(offsetof(consumer_struct_t, ip4) ==                      \
                       offsetof(bb_mdns_cache_entry_t, ip4),                 \
                   #consumer_struct_t ".ip4 must match bb_mdns_cache_entry_t layout"); \
    _Static_assert(offsetof(consumer_struct_t, port) ==                     \
                       offsetof(bb_mdns_cache_entry_t, port),                \
                   #consumer_struct_t ".port must match bb_mdns_cache_entry_t layout")

#ifdef __cplusplus
}
#endif
