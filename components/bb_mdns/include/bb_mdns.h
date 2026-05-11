#pragma once

#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef ESP_PLATFORM

#include <stdbool.h>

// Initialize mDNS. Registers got-IP callback on bb_wifi. Idempotent.
void bb_mdns_init(void);

// Tear down mDNS. Stops the service and any timers. Idempotent — safe
// to call when not started. Pair with bb_mdns_init() to live-toggle
// mdns at runtime (e.g. from a settings handler).
void bb_mdns_deinit(void);

// Set mDNS hostname. Must be called after bb_mdns_init().
void bb_mdns_set_hostname(const char *hostname);

// Set mDNS service type (e.g. "_taipanminer"). Must be called before bb_mdns_init().
// Defaults to "_bsp" if not set.
void bb_mdns_set_service_type(const char *service_type);

// Set mDNS instance name (e.g. "TaipanMiner"). Must be called before bb_mdns_init().
// Defaults to "BSP Device" if not set.
void bb_mdns_set_instance_name(const char *instance_name);

// Check if mDNS has been started.
bool bb_mdns_started(void);

// Get the running mDNS hostname (cached on init). Returns the string when
// started (e.g. "bsp-device-c718"), NULL when not started or before init.
const char *bb_mdns_get_hostname(void);

#endif /* ESP_PLATFORM */

// Start (or restart) mDNS synchronously. Used by consumers that have just
// called bb_mdns_deinit() and want to re-arm without waiting for the next
// wifi got-IP event. No-op when mdns is already started. Safe to call
// before bb_mdns_init() — becomes a no-op until init has run.
void bb_mdns_start(void);

// Sanitize and build RFC 1035-compliant hostname label: lowercase [a-z0-9], collapse/trim dashes, cap at 63 chars.
void bb_mdns_build_hostname(const char *prefix, const char *suffix, char *out, size_t out_size);

// Update a single TXT record on the registered service. Safe to call after
// init; no-op if mdns isn't started or before service start.
// Use for fields that change post-init (version after OTA, state transitions, etc.).
void bb_mdns_set_txt(const char *key, const char *value);

/// Force an unsolicited re-announce of the local mDNS service.
/// Useful after a burst of bb_mdns_set_txt calls — observers' IDF
/// caches sometimes miss TXT-only updates without a full re-announce.
/// Safe before service start (becomes a no-op until start).
void bb_mdns_announce(void);

typedef struct {
    char *key;
    char *value;
} bb_mdns_txt_t;

/* Bounds for inlined string fields below. Sized to match the underlying
 * mDNS event slot buffers; safe to copy by value. */
#define BB_MDNS_INSTANCE_NAME_MAX 64
#define BB_MDNS_HOSTNAME_MAX      64
#define BB_MDNS_IP4_MAX           16

typedef struct {
    char     instance_name[BB_MDNS_INSTANCE_NAME_MAX]; /* e.g. "TaipanMiner-abc123" */
    char     hostname[BB_MDNS_HOSTNAME_MAX];           /* e.g. "tdongles3-1.local", "" if unknown */
    char     ip4[BB_MDNS_IP4_MAX];                     /* dotted-quad, "" if v6-only / unresolved */
    uint16_t port;
    const bb_mdns_txt_t *txt;       /* TXT key/value pairs (read-only view, callback-scoped) */
    size_t              txt_count;
} bb_mdns_peer_t;

/* Callbacks fire from the bb_mdns dispatch task (NOT the IDF mDNS task,
 * NOT the caller's task).
 *
 * Scalar string fields (instance_name, hostname, ip4) are inlined into the
 * struct, so a `bb_mdns_peer_t` copy is sufficient to retain them. Empty
 * fields are zero-length strings, never NULL.
 *
 * The `txt` array view is owned by bb_mdns and remains valid ONLY for the
 * duration of the callback — consumers that need to retain TXT records
 * must copy them. on_removed fires with the inlined instance_name only.
 *
 * Freshness: IDF's mDNS browser only invokes its notify callback on PTR
 * state changes (initial discovery, removal). Active advertisers
 * re-announcing with the same TTL are silently absorbed by IDF's cache,
 * so on_peer would NEVER fire again for stable peers without
 * intervention. To surface "still alive", bb_mdns periodically deletes +
 * re-creates each browse subscription (interval set by
 * CONFIG_BB_MDNS_BROWSE_REFRESH_INTERVAL_S, default 60s, 0 disables);
 * each refresh reissues a PTR query and produces fresh on_peer
 * notifications for any responder. Peers that don't reply within the
 * new query window fire on_removed before any subsequent on_peer; this
 * is normal refresh churn, not a real disconnect. Consumers needing
 * finer freshness can layer their own bb_mdns_query_txt / per-peer
 * probe on top. */
typedef void (*bb_mdns_peer_cb)(const bb_mdns_peer_t *peer, void *ctx);
typedef void (*bb_mdns_peer_removed_cb)(const char *instance_name, void *ctx);

/* Result of an async TXT query. Same field-ownership contract as
 * bb_mdns_peer_t: scalar strings are inlined and copy-safe; the `txt`
 * view is callback-scoped. err is BB_OK on success; on failure (timeout,
 * no response, IDF error) other fields are unspecified except err. */
typedef struct {
    bb_err_t err;
    char     instance_name[BB_MDNS_INSTANCE_NAME_MAX];
    char     hostname[BB_MDNS_HOSTNAME_MAX];
    char     ip4[BB_MDNS_IP4_MAX];
    uint16_t port;
    const bb_mdns_txt_t *txt;
    size_t              txt_count;
} bb_mdns_query_result_t;

typedef void (*bb_mdns_query_cb)(const bb_mdns_query_result_t *result, void *ctx);

/* Async TXT query. Posts the query to bb_mdns's worker; the caller's
 * task does NOT block. Callback fires from the bb_mdns dispatch task
 * with result.err = BB_OK on success or an error code on failure
 * (timeout, no response). timeout_ms is per-query (caps how long the
 * worker waits in mdns_query_txt). instance_name/service/proto strings
 * must remain valid until the callback fires.
 *
 * Returns BB_OK if the query was queued, BB_ERR_NO_SPACE if the query
 * queue is full (caller should back off), BB_ERR_INVALID_ARG on NULL
 * required args, BB_ERR_INVALID_STATE if mdns isn't initialized.
 *
 * SAFE TO CALL from any task including ISR-deferred contexts and
 * esp_timer service task (the original buggy path). */
bb_err_t bb_mdns_query_txt(const char *instance_name,
                           const char *service,
                           const char *proto,
                           uint32_t timeout_ms,
                           bb_mdns_query_cb cb,
                           void *ctx);

/* Start a continuous async browse for `_<service>._<proto>` on the LAN.
 * Either callback may be NULL. ctx is opaque user data passed through.
 * Returns BB_OK on success; BB_ERR_INVALID_ARG on NULL service/proto;
 * BB_ERR_INVALID_STATE if mdns isn't initialized; BB_ERR_NO_SPACE on
 * subscription-table exhaustion. Idempotent: calling start twice for the
 * same (service, proto) replaces the prior callbacks. */
bb_err_t bb_mdns_browse_start(const char *service, const char *proto,
                              bb_mdns_peer_cb on_peer,
                              bb_mdns_peer_removed_cb on_removed,
                              void *ctx);

/* Stop a previously-started browse. Returns BB_OK if stopped or already
 * unstarted (idempotent). */
bb_err_t bb_mdns_browse_stop(const char *service, const char *proto);
