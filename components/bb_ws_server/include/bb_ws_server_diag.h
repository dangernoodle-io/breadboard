#pragma once

// bb_ws_server_diag — the "websocket" bb_diag section (GET
// /api/diag/websocket), B1-1077 PR-3a. Replaces the prior hand-rolled exact
// route (platform/espidf/bb_ws_server/bb_ws_server_diag.c, deleted this PR)
// with a bb_diag_register_section() fill adapter, same shape as
// bb_diag_meminfo's thin shim. Compiled unconditionally (independent of
// CONFIG_HTTPD_WS_SUPPORT — bb_ws_server_open_count() returns 0 when WS
// support is compiled out, so the section still reports a valid, if
// trivial, snapshot instead of failing to exist).

#include "bb_diag_section.h"
#include "bb_serialize.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Section snapshot — single scalar, mirrors the prior route's sole field.
typedef struct {
    int64_t open_connections;
} bb_ws_server_diag_snap_t;

extern const bb_serialize_desc_t bb_ws_server_diag_desc;

// Hand-authored JSON Schema for the section's GET response (B1-1180 PR-1) --
// makes the "websocket" section VISIBLE to bb_openapi_emit() via
// bb_diag_section_t.describe_route (wired in this file's own
// bb_ws_server_diag_register()). On-device (NOT host-gated): flashed in
// every build, unlike the BB_SERIALIZE_META_HOST-gated meta table below. See
// test/test_host/test_bb_ws_server_diag_meta_golden.c for the byte-fidelity
// proof against bb_ws_server_diag_meta.
extern const char *const bb_ws_server_diag_schema;

// Fill hook (bb_diag_fill_fn signature) — pure/portable, delegates to
// bb_ws_server_open_count() (works on host + ESP-IDF). `args` is unused
// (this section declares no query_keys). Returns BB_ERR_INVALID_ARG if dst
// is NULL.
bb_err_t bb_ws_server_diag_fill(void *dst, const bb_diag_fill_args_t *args);

// bb_serialize_desc_meta_t companion (B1-1180 PR-1) -- co-located JSON
// Schema docs/validation table for bb_ws_server_diag_desc above, proving
// bb_ws_server_diag_schema's byte-fidelity. BB_SERIALIZE_META_HOST is a
// host-only define (set by the PlatformIO native env; see platformio.ini)
// -- NEVER set by the ESP-IDF/device build.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_ws_server_diag_meta;
#endif /* BB_SERIALIZE_META_SHIP */

#ifdef ESP_PLATFORM
// Registers this section as "websocket" (GET /api/diag/websocket) via
// bb_diag_register_section(). Composition-time-only, once.
// bbtool:init tier=regular fn=bb_ws_server_diag_register
bb_err_t bb_ws_server_diag_register(void);
#endif

#ifdef __cplusplus
}
#endif
