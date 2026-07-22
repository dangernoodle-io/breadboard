#pragma once

// Private: v2 wire descriptor for the /api/health ROOT-IDENTITY FIELD SLICE
// (B1-767 PR-6; LIVE since B1-1100 PR-5).
//
// bb_serialize_desc_t SSOT reproducing the on-wire bytes byte-for-byte for
// the root-identity fields ONLY (see test/test_host/test_v2_golden.c) --
// the fields the ESP-IDF /api/health handler
// (platform/espidf/bb_health/bb_health.c) gathers directly from
// bb_wifi/bb_mdns and hands to bb_health_compose_and_stream()
// (bb_health_compose_priv.h) as a RAW group, merged flat at the document
// root ahead of the registered sections ("mqtt", "temp", ...), which
// compose as a named OBJECT group in the same call. This descriptor is
// NOT byte-for-byte fidelity for the FULL /api/health document on its
// own -- full document = this root slice + the registered sections; see
// test_v2_golden.c's full-document golden for that composed shape.

#include "bb_serialize.h"

#include <stdbool.h>

// network subsection -- field order matches health_handler()'s own gather
// step (platform/espidf/bb_health/bb_health.c): ssid, bssid, ip, connected,
// then that same gather's "mdns" append. (Historical note: this order used
// to also match a now-deleted bb_json_t emitter, bb_wifi_emit_status --
// B1-1149 -- but health_handler() never called it; it always gathered these
// fields directly.)
typedef struct {
    char                  ssid[33];
    char                  bssid[18];
    char                  ip[16];
    bool                  connected;
    bb_serialize_str_n_t  mdns;  // .ptr == NULL -> emit_null
} bb_health_net_wire_t;

// Root -- field order matches health_handler(): ok, network. validated
// dropped (B1-977, bb_board dissolution -- BREAKING CHANGE: GET /api/health
// no longer returns "validated").
typedef struct {
    bool                  ok;
    bb_health_net_wire_t  network;
} bb_health_wire_t;

extern const bb_serialize_desc_t bb_health_wire_desc;
