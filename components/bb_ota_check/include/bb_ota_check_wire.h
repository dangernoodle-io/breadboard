#pragma once

// bb_ota_check_wire — PUBLIC bb_serialize_desc_t (SSOT) for the
// "update.available" bb_data key (B1-1045 PR-2/PR-4, cutover composition-root
// ownership decision KB 1454). The composition root
// (examples/floor/main/floor_app.c, PR-4) owns wiring this descriptor to
// bb_data via bb_data_bind().
//
// bb_ota_check_snap_t is REUSED DIRECTLY (moved here from
// src/bb_ota_check_internal.h, which now includes this header for the
// type) -- no widened parallel struct is needed: ts/last_check_ts are
// already int64_t and every other field is bool/char, so nothing in the
// existing snapshot layout violates bb_serialize_walk()'s fixed-8-byte
// BB_TYPE_I64 memcpy contract (see bb_serialize_walk.c). This relocation
// (review finding 2) makes bb_ota_check_snap_t's layout public API: it is
// now read directly by bb_ota_check_wire.h's own bb_ota_check_gather(), not
// just by bb_ota_check_common.c internally.
//
// Mirrors bb_ota_check_serialize()'s `{"current","latest","download_url",
// "available","ts","last_check_ok","enabled","outcome"[,"last_check_ts"]}`
// shape field-for-field.

#include "bb_serialize.h"

#include "bb_core.h"

#include <stdbool.h>
#include <stdint.h>

// Single source of truth for the update-check bb_cache/event topic name
// (moved here from src/bb_ota_check_internal.h so bb_ota_check_gather()
// below can reference it without a private-header include cycle).
#define BB_OTA_CHECK_TOPIC "update.available"

// ---------------------------------------------------------------------------
// Canonical owned snapshot for the update.available bb_cache topic.
// Shared between SSE (bb_ota_check_common.c) and REST (bb_ota_check_espidf.c).
// Included by test/test_host/test_bb_cache_fidelity.c.
// ---------------------------------------------------------------------------
typedef struct {
    char    current[24];       // matches bb_ota_check_status_t.current
    char    latest[24];        // matches bb_ota_check_status_t.latest
    char    download_url[256]; // URL_MAX
    bool    available;
    int64_t ts;                // wall-clock seconds at publish time
    bool    last_check_ok;
    bool    enabled;
    char    outcome[24];       // outcome_str result; longest = "check_on_apply" (14)
    int64_t last_check_ts;     // epoch-s from last_check_us; 0 = omit
} bb_ota_check_snap_t;

_Static_assert(sizeof(((bb_ota_check_snap_t *)0)->ts) == 8,
               "bb_ota_check_snap_t.ts must be exactly 8 bytes for BB_TYPE_I64");
_Static_assert(sizeof(((bb_ota_check_snap_t *)0)->last_check_ts) == 8,
               "bb_ota_check_snap_t.last_check_ts must be exactly 8 bytes for BB_TYPE_I64");

extern const bb_serialize_desc_t bb_ota_check_wire_desc;

// Portable (no ESP-IDF dep): reads the update.available bb_cache entry via
// bb_cache_get_raw() into `dst` -- a plain copy, no widening needed. Returns
// BB_ERR_INVALID_ARG if dst is NULL; otherwise propagates bb_cache_get_raw()'s
// own return (BB_ERR_NOT_FOUND if the update.available key isn't registered
// yet).
bb_err_t bb_ota_check_gather(bb_ota_check_snap_t *dst);
