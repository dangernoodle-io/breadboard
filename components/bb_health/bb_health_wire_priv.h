#pragma once

// Private: v2 wire descriptor for the /api/health ROOT-IDENTITY FIELD SLICE
// (B1-767 PR-6).
//
// bb_serialize_desc_t SSOT reproducing TODAY's on-wire bytes byte-for-byte
// for the root-identity fields ONLY (see test/test_host/test_v2_golden.c)
// -- the fields health_handler() emits INLINE, BEFORE it calls
// bb_response_build_get(&reg, root) to append the dynamically-registered
// sections (today: "mqtt", "temp"). This descriptor is NOT byte-for-byte
// fidelity for the FULL /api/health document -- full document = this root
// slice + the registered sections. The deferred cutover must still run the
// section registry (bb_response_build_get) after rendering this root
// descriptor; serving sections themselves as descriptors is a separate,
// later piece of work (composed-section conversion, see roadmap epic
// B1-786). ADDITIVE only -- not yet wired into the live /api/health handler
// (platform/espidf/bb_health/bb_health.c), which still emits via bb_json_t
// directly.

#include "bb_serialize.h"

#include <stdbool.h>

// network subsection -- field order matches bb_wifi_emit_status()
// (platform/host/bb_wifi_http/bb_wifi_http_emit.c): ssid, bssid, ip,
// connected, then bb_health.c's own "mdns" append.
typedef struct {
    char                  ssid[33];
    char                  bssid[18];
    char                  ip[16];
    bool                  connected;
    bb_serialize_str_n_t  mdns;  // .ptr == NULL -> emit_null
} bb_health_net_wire_t;

// Root -- field order matches health_handler(): ok, validated, network.
typedef struct {
    bool                  ok;
    bool                  validated;
    bb_health_net_wire_t  network;
} bb_health_wire_t;

extern const bb_serialize_desc_t bb_health_wire_desc;
