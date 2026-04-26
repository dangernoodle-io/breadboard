#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM

#include <stdbool.h>
#include "bb_nv.h"

// Initialize mDNS. Registers got-IP callback on bb_wifi. Idempotent.
void bb_mdns_init(void);

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

#endif /* ESP_PLATFORM */

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

typedef struct {
    const char         *instance_name;   /* e.g. "TaipanMiner-abc123" */
    const char         *hostname;        /* e.g. "tdongles3-1.local" */
    const char         *ip4;             /* first IPv4 dotted-quad, NULL if v6-only */
    uint16_t            port;
    const bb_mdns_txt_t *txt;            /* TXT key/value pairs (read-only view) */
    size_t              txt_count;
} bb_mdns_peer_t;

/* Callbacks fire from the mdns task; the bb_mdns_peer_t and its strings are
 * valid only for the duration of the call. Consumers must copy what they
 * need before returning. on_removed fires with just the instance_name. */
typedef void (*bb_mdns_peer_cb)(const bb_mdns_peer_t *peer, void *ctx);
typedef void (*bb_mdns_peer_removed_cb)(const char *instance_name, void *ctx);

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
