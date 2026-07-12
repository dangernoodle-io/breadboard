#pragma once

// Private: v2 wire descriptor for the /api/info ROOT-IDENTITY FIELD SLICE
// (B1-767 PR-6).
//
// bb_serialize_desc_t SSOT reproducing TODAY's on-wire bytes byte-for-byte
// for the root-identity fields ONLY (see test/test_host/test_v2_golden.c)
// -- the fields info_handler() emits INLINE, BEFORE it calls
// bb_response_build_get(&reg, root) to append the dynamically-registered
// sections (today: "build", plus display/led/ntp/diag when those
// components are composed in). This descriptor is NOT byte-for-byte
// fidelity for the FULL /api/info document -- full document = this root
// slice + the registered sections. The deferred cutover must still run the
// section registry (bb_response_build_get) after rendering this root
// descriptor; serving sections themselves as descriptors is a separate,
// later piece of work (composed-section conversion, see roadmap epic
// B1-786). ADDITIVE only -- not yet wired into the live /api/info handler
// (platform/espidf/bb_info/bb_info.c), which still emits via bb_json_t
// directly.
//
// Integer fields are typed BB_TYPE_I64/BB_TYPE_U64, NOT BB_TYPE_F64: today's
// handler renders them via cJSON bb_json_obj_set_number((double)x), which
// cJSON prints as a bare integer (e.g. "0", "1704067200") when the value is
// integer-valued -- bb_serialize_json's F64 backend instead emits a fixed
// 6-decimal float ("0.000000"), which would NOT match today's bytes.
//
// ota_ready is intentionally OMITTED from this v2 descriptor: it is
// conditionally emitted today (BB_INFO_EMIT_OTA_READY, gated on
// CONFIG_BB_OTA_PULL_AUTOREGISTER / CONFIG_BB_OTA_BOOT_STATUS_HTTP), and is
// out of scope for the canonical golden captured here.

#include "bb_serialize.h"

#include <stdbool.h>
#include <stdint.h>

// Field order matches platform/espidf/bb_info/bb_info.c's info_handler():
// mac, ota_validated, time_valid, boot_epoch_s, time_source, hostname,
// capabilities.
typedef struct {
    char                   mac[18];
    bool                   ota_validated;
    bool                   time_valid;
    int64_t                boot_epoch_s;    // I64, NOT F64 -- see header comment
    char                   time_source[8];  // "sntp" / "none"
    bb_serialize_str_n_t   hostname;        // .ptr == NULL -> emit_null
    bb_serialize_arr_str_t capabilities;    // count == 0 -> []
} bb_info_wire_t;

extern const bb_serialize_desc_t bb_info_wire_desc;
