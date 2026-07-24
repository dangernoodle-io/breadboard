#pragma once

// bb_display_info_wire — PUBLIC bb_serialize_desc_t (SSOT) for the
// "health.display" bb_cache topic (B1-1045 PR-2, cutover composition-root
// ownership decision KB 1454), plus the bb_data bind cut over onto it
// (B1-1146a, the last piece of B1-1053: kills the legacy bb_json bb_cache
// serializer bb_display_serialize() had, deleted along with it --
// bb_display_register_info() (both platform/espidf/bb_display and
// platform/host/bb_display) now self-binds via bb_display_info_bind() below
// right after registering the bb_cache entry, mirroring bb_diag_boot_bind()/
// bb_ota_check_bind()'s composition-time self-bind pattern). NO REST/SSE
// render seam is added by this PR: health.display's REST exposure is being
// rehomed to system.display under bb_system's diag endpoint (B1-1150) --
// this bind is purely what lets a FUTURE reader there resolve the key via
// bb_data_render(); see bb_display_info_gather()'s doc comment below for the
// refresh caveat that reader will need.
//
// Mirrors the deleted bb_display_serialize()'s `{"present"[,"panel","width",
// "height","enabled"]}` shape field-for-field.
// width/height are widened from `int` to a fixed int64_t -- bb_serialize_
// walk()'s BB_TYPE_I64 case always memcpy()s a fixed 8 bytes at the
// descriptor offset (see bb_serialize_walk.c); pointing a BB_TYPE_I64 field
// at a narrower `int` would read past it.

#include "bb_serialize.h"

#include "bb_core.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool    present;
    char    panel[32];
    int64_t width;
    int64_t height;
    bool    enabled;
} bb_display_info_wire_t;

_Static_assert(sizeof(((bb_display_info_wire_t *)0)->width) == 8,
               "bb_display_info_wire_t.width must be exactly 8 bytes for BB_TYPE_I64");
_Static_assert(sizeof(((bb_display_info_wire_t *)0)->height) == 8,
               "bb_display_info_wire_t.height must be exactly 8 bytes for BB_TYPE_I64");

extern const bb_serialize_desc_t bb_display_info_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1179) -- co-located JSON Schema
// docs/validation table for bb_display_info_wire_desc above, same #if-gated
// pattern as bb_diag_boot_wire.h's exemplar. BB_SERIALIZE_META_HOST is a
// host-only define (set by the PlatformIO native env; see platformio.ini)
// -- NEVER set by the ESP-IDF/device build, so this declaration (and its
// definition in bb_display_info_wire.c) compiles to nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_display_info_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// Portable (no ESP-IDF dep): reads the health.display bb_cache entry via
// bb_cache_get_raw() and widens width/height into `dst`. Returns
// BB_ERR_INVALID_ARG if dst is NULL; otherwise propagates bb_cache_get_raw()'s
// own return (BB_ERR_NOT_FOUND if the health.display key isn't registered
// yet).
//
// REFRESH CAVEAT FOR ANY FUTURE REST/SSE READER (B1-1119/B1-1150 --
// bb_system's diag endpoint, once health.display is rehomed to
// system.display): this gather is a PURE PASS-THROUGH over
// bb_cache_get_raw() -- VERIFIED against its actual body above, not assumed
// -- it does not itself re-read the live display backend
// (bb_display_backend_name()/bb_display_width()/height()) or the
// bb_settings display-enabled flag. Unlike update.available/diag.boot
// (which have a periodic republish keeping their bb_cache entry warm),
// health.display's bb_cache entry is written EXACTLY ONCE, at
// bb_display_info_register_init() (platform/espidf/bb_display/
// bb_display_info.c) -- `enabled` can change afterward at runtime via
// bb_settings_display_enabled_set() with no corresponding bb_cache_update()
// call. So ANY reader that renders this key (directly via this gather, or
// indirectly via bb_data_render() against the binding below) MUST re-run
// bb_display_info.c's make_snap() + bb_cache_update() immediately before
// rendering, or the result freezes at whatever was last published --
// this refresh is NOT optional/redundant, unlike PR3's finding for
// update.available's gather (bb_ota_check_common.c), which had a periodic
// republish keeping it warm; health.display has no such periodic producer.
bb_err_t bb_display_info_gather(bb_display_info_wire_t *dst);

// Binds the "health.display" bb_data key against bb_display_info_gather()
// above (B1-1146a, mirrors bb_diag_boot_bind()'s/bb_ota_check_bind()'s
// composition-time self-bind pattern). Portable (no bb_http_handle_t/ESP-IDF
// dependency) -- called by bb_display_register_info() on both platforms
// (platform/espidf/bb_display/bb_display_info.c, platform/host/bb_display/
// bb_display_info.c) right after bb_cache_register() succeeds, so a future
// bb_data_render() caller (B1-1119/B1-1150) can resolve the key. Also
// callable directly by host tests after bb_data_test_reset(). Idempotent:
// bb_data_bind() re-binding an already-bound key overrides it in place.
bb_err_t bb_display_info_bind(void);
