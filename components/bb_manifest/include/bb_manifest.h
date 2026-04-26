#pragma once

// Opt-in device-manifest endpoint.
// Consumer firmware links this component only by adding `bb_manifest` to a
// REQUIRES/PRIV_REQUIRES line in one of its own components' idf_component_register
// calls. The runtime endpoint GET /api/manifest serves a JSON document
// describing the consumer-registered NVS keyspace and mDNS TXT keyset.

#include "bb_http.h"
#include "bb_json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Description of an NVS key. All pointers must outlive the registration
// (typically static const literals).
typedef struct {
    const char *key;
    const char *type;             // "str" | "u8" | "u16" | "u32" | "i32" | "blob" | "bool"
    const char *default_;         // string repr of default; NULL if no default
    int         max_len;          // for str/blob; 0 otherwise
    const char *desc;
    bool        reboot_required;
    bool        provisioning_only;
} bb_manifest_nv_t;

// Description of an mDNS TXT key.
typedef struct {
    const char *key;
    const char *desc;
    const char *values;           // "|"-separated enum (e.g. "mining|idle|ota"); NULL = free-form
} bb_manifest_mdns_t;

// Register a table of NVS keys under a namespace. Pointer fields and the
// table itself must outlive the registration. Multiple namespaces may be
// registered (each via its own call).
bb_err_t bb_manifest_register_nv(const char *namespace,
                                 const bb_manifest_nv_t *keys, size_t n);

// Register a table of mDNS TXT keys under a service (e.g. "_taipanminer._tcp").
bb_err_t bb_manifest_register_mdns(const char *service,
                                   const bb_manifest_mdns_t *keys, size_t n);

// Build the manifest JSON document. Caller must free with bb_json_free().
bb_json_t bb_manifest_emit(void);

// ESP-IDF: register GET /api/manifest. Handler invokes bb_manifest_emit(),
// serializes to JSON, returns Content-Type: application/json.
bb_err_t bb_manifest_register_route(bb_http_handle_t server);

// Test helpers — clear all registrations.
void bb_manifest_clear(void);

#ifdef __cplusplus
}
#endif
