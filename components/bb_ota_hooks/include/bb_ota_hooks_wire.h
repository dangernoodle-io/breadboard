#pragma once

// bb_ota_hooks_wire — PUBLIC bb_serialize_desc_t (SSOT) for the
// "ota.progress" bb_data key (B1-1045 PR-2, cutover composition-root
// ownership decision KB 1454). The composition root
// (examples/floor/main/floor_app.c, PR-4) owns wiring this descriptor to
// bb_data via bb_data_bind().
//
// Mirrors bb_ota_progress_json()'s `{"via","state","pct"}` shape
// (bb_ota_hooks.h) field-for-field. `pct` is widened from `int` to a fixed
// int64_t -- bb_serialize_walk()'s BB_TYPE_I64 case always memcpy()s a fixed
// 8 bytes at the descriptor offset (see bb_serialize_walk.c); pointing a
// BB_TYPE_I64 field at a narrower `int` would read past it.

#include "bb_serialize.h"

#include "bb_core.h"

#include <stdint.h>

typedef struct {
    char    via[16];
    char    state[12];
    int64_t pct;
} bb_ota_hooks_wire_t;

_Static_assert(sizeof(((bb_ota_hooks_wire_t *)0)->pct) == 8,
               "bb_ota_hooks_wire_t.pct must be exactly 8 bytes for BB_TYPE_I64");

extern const bb_serialize_desc_t bb_ota_hooks_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-2b-i-2) -- co-located JSON
// Schema docs/validation table for bb_ota_hooks_wire_desc above, same
// #if-gated pattern as bb_wifi_http_wire_priv.h's exemplar (B1-1059 PR-2a).
// BB_SERIALIZE_META_HOST is a host-only define (set by the PlatformIO native
// env; see platformio.ini) -- NEVER set by the ESP-IDF/device build, so this
// declaration (and its definition in bb_ota_hooks_wire.c) compiles to nothing
// on-device.
#if defined(BB_SERIALIZE_META_HOST)
#include "bb_serialize_meta.h"

extern const bb_serialize_desc_meta_t bb_ota_hooks_wire_meta;
#endif /* BB_SERIALIZE_META_HOST */

// Portable (no ESP-IDF dep): fills `dst` from bb_ota_hooks' last-emitted
// progress stash (s_last_phase/s_last_pct/s_last_via in bb_ota_hooks.c,
// updated unconditionally by bb_ota_emit_progress()). Returns
// BB_ERR_INVALID_ARG if dst is NULL; otherwise BB_OK, even before the first
// bb_ota_emit_progress() call (reads the stash's BB_OTA_PHASE_FAIL/0/""
// initial state).
bb_err_t bb_ota_hooks_gather(bb_ota_hooks_wire_t *dst);
